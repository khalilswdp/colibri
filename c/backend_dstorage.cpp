/* DirectStorage transport for the VRAM depot (Windows-only): shard-file
 * regions DMA straight into VRAM, no engine-RAM transit, ~zero CPU.
 *
 * Design notes earned the hard way on the reference box (RTX 5080, display on
 * the iGPU):
 *  - NEVER take the default D3D12 adapter: it is the DISPLAY adapter, which
 *    here is the Intel iGPU — CUDA cannot import a foreign device's resources
 *    ("operation not supported"). Enumerate DXGI and pick VendorId 0x10DE.
 *  - The arena is SPLIT into <=2 GiB chunks: destination-buffer offsets past
 *    4 GiB were observed to WRAP on this stack (writes landed at off mod
 *    4 GiB, deterministically — zeros high, foreign bytes low). Small-offset
 *    requests only; the engine pads entries so none straddles a chunk.
 *  - Every chunk gets D3D12_RESIDENCY_PRIORITY_MAXIMUM: a demoted page means
 *    a torn DMA write under VRAM pressure.
 *  - Requests larger than the staging buffer fail TOO_LARGE (GLM's MTP
 *    experts are ~38 MB int8): staging raised to 128 MB AND reads chunked
 *    at 16 MB.
 *  - dstorage.dll loads DYNAMICALLY so coli_cuda.dll works without it.
 *
 * Threading contract: single-threaded producer (startup fill loop). */
#ifdef _WIN32
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <cuda_runtime.h>
#include <dstorage.h>
#include <cstdio>
#include <cstring>

#define DSEXPORT extern "C" __declspec(dllexport)

#define DS_CHUNK (2ull<<30)
#define DS_MAX_CHUNKS 8

static IDStorageFactory *g_fac;
static IDStorageQueue   *g_q;
static ID3D12Device     *g_d3d;
static ID3D12Fence      *g_fence;
static UINT64            g_fval;
static HANDLE            g_ev;
static IDXGIAdapter3    *g_ad3;
static ID3D12Resource   *g_res[DS_MAX_CHUNKS];
static cudaExternalMemory_t g_ext[DS_MAX_CHUNKS];
static void             *g_ptr[DS_MAX_CHUNKS];
static int               g_nchunks;
static unsigned long long g_arena_bytes;
static struct { char path[1024]; IDStorageFile *f; } g_files[512];
static int g_nfiles;
static int g_state;      /* 0 = untried, 1 = ready, -1 = failed */

DSEXPORT int coli_cuda_ds_init(void){
    if(g_state) return g_state>0;
    g_state=-1;
    HMODULE dm=LoadLibraryExA("dstorage.dll",NULL,
        LOAD_LIBRARY_SEARCH_APPLICATION_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
    if(!dm){ fprintf(stderr,"[DS] dstorage.dll not found next to the engine\n"); return 0; }
    typedef HRESULT (WINAPI *PFGF)(REFIID,void**);
    PFGF gf=(PFGF)GetProcAddress(dm,"DStorageGetFactory");
    if(!gf){ fprintf(stderr,"[DS] DStorageGetFactory missing in dstorage.dll\n"); return 0; }
    IDXGIFactory4 *dxf=nullptr;
    if(FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&dxf)))) return 0;
    IDXGIAdapter1 *ad=nullptr;
    for(UINT i=0; dxf->EnumAdapters1(i,&ad)==S_OK; ++i){
        DXGI_ADAPTER_DESC1 d; ad->GetDesc1(&d);
        if(d.VendorId==0x10DE) break;
        ad->Release(); ad=nullptr;
    }
    if(!ad){ fprintf(stderr,"[DS] no NVIDIA adapter in DXGI enumeration\n"); return 0; }
    ad->QueryInterface(IID_PPV_ARGS(&g_ad3));
    if(FAILED(D3D12CreateDevice(ad,D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&g_d3d)))) return 0;
    if(FAILED(gf(IID_PPV_ARGS(&g_fac)))) return 0;
    g_fac->SetStagingBufferSize(128*1048576u);
    DSTORAGE_QUEUE_DESC qd{};
    qd.SourceType=DSTORAGE_REQUEST_SOURCE_FILE;
    qd.Capacity=DSTORAGE_MAX_QUEUE_CAPACITY;
    qd.Priority=DSTORAGE_PRIORITY_NORMAL;
    qd.Device=g_d3d;
    if(FAILED(g_fac->CreateQueue(&qd,IID_PPV_ARGS(&g_q)))) return 0;
    if(FAILED(g_d3d->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&g_fence)))) return 0;
    g_ev=CreateEventA(NULL,FALSE,FALSE,NULL);
    if(!g_ev) return 0;
    g_state=1;
    return 1;
}

/* What the OS will let this process keep RESIDENT in VRAM right now. */
DSEXPORT unsigned long long coli_cuda_ds_budget(void){
    if(g_state!=1 || !g_ad3) return 0;
    DXGI_QUERY_VIDEO_MEMORY_INFO mi{};
    if(FAILED(g_ad3->QueryVideoMemoryInfo(0,DXGI_MEMORY_SEGMENT_GROUP_LOCAL,&mi))) return 0;
    return mi.Budget>mi.CurrentUsage ? mi.Budget-mi.CurrentUsage : 0;
}

DSEXPORT void *coli_cuda_ds_arena_alloc(unsigned long long bytes){
    if(g_state!=1 || g_nchunks) return NULL;
    unsigned long long left=bytes;
    while(left && g_nchunks<DS_MAX_CHUNKS){
        unsigned long long cb = left<DS_CHUNK ? left : DS_CHUNK;
        D3D12_HEAP_PROPERTIES hp{}; hp.Type=D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width=cb; rd.Height=1; rd.DepthOrArraySize=1; rd.MipLevels=1;
        rd.Format=DXGI_FORMAT_UNKNOWN; rd.SampleDesc.Count=1;
        rd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR; rd.Flags=D3D12_RESOURCE_FLAG_NONE;
        int k=g_nchunks;
        if(FAILED(g_d3d->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_SHARED,&rd,
            D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&g_res[k])))){
            fprintf(stderr,"[DS] arena chunk %d alloc failed (%.2f GB)\n",k,cb/1e9); break; }
        { ID3D12Device1 *d1=nullptr;
          if(SUCCEEDED(g_d3d->QueryInterface(IID_PPV_ARGS(&d1)))){
              ID3D12Pageable *pg=g_res[k];
              D3D12_RESIDENCY_PRIORITY pr=D3D12_RESIDENCY_PRIORITY_MAXIMUM;
              d1->SetResidencyPriority(1,&pg,&pr);
              d1->Release();
          } }
        HANDLE sh=nullptr;
        if(FAILED(g_d3d->CreateSharedHandle(g_res[k],nullptr,GENERIC_ALL,nullptr,&sh))) break;
        cudaExternalMemoryHandleDesc md{};
        md.type=cudaExternalMemoryHandleTypeD3D12Resource;
        md.handle.win32.handle=sh; md.size=cb; md.flags=cudaExternalMemoryDedicated;
        cudaError_t ce=cudaImportExternalMemory(&g_ext[k],&md);
        CloseHandle(sh);
        if(ce!=cudaSuccess){ fprintf(stderr,"[DS] CUDA import failed: %s\n",cudaGetErrorString(ce)); break; }
        cudaExternalMemoryBufferDesc bd{}; bd.offset=0; bd.size=cb;
        if(cudaExternalMemoryGetMappedBuffer(&g_ptr[k],g_ext[k],&bd)!=cudaSuccess) break;
        g_nchunks++; g_arena_bytes+=cb; left-=cb;
    }
    if(left) fprintf(stderr,"[DS] arena short: %.2f of %.2f GB\n",g_arena_bytes/1e9,bytes/1e9);
    return g_nchunks? g_ptr[0] : NULL;
}

/* Device pointer for a logical arena offset (chunked mapping). */
DSEXPORT void *coli_cuda_ds_arena_ptr(unsigned long long off){
    unsigned long long k=off/DS_CHUNK;
    if(k>=(unsigned long long)g_nchunks) return NULL;
    return (char*)g_ptr[k]+(off%DS_CHUNK);
}

static IDStorageFile *ds_file(const char *path){
    for(int i=0;i<g_nfiles;i++) if(!strcmp(g_files[i].path,path)) return g_files[i].f;
    if(g_nfiles>=512) return NULL;
    wchar_t w[1024];
    if(!MultiByteToWideChar(CP_UTF8,0,path,-1,w,1024)) return NULL;
    IDStorageFile *f=nullptr;
    if(FAILED(g_fac->OpenFile(w,IID_PPV_ARGS(&f)))){
        fprintf(stderr,"[DS] OpenFile failed: %s\n",path); return NULL; }
    strncpy(g_files[g_nfiles].path,path,1023);
    g_files[g_nfiles].path[1023]=0;
    g_files[g_nfiles].f=f; g_nfiles++;
    return f;
}

DSEXPORT int coli_cuda_ds_read(const char *path, unsigned long long off,
                               unsigned long long size, unsigned long long dst_off){
    if(g_state!=1 || !g_nchunks || !size) return 0;
    if(dst_off/DS_CHUNK != (dst_off+size-1)/DS_CHUNK){
        fprintf(stderr,"[DS] read straddles arena chunk (engine bug)\n"); return 0; }
    IDStorageFile *f=ds_file(path);
    if(!f) return 0;
    const unsigned long long CH=16*1048576ull;    /* per-request cap (staging) */
    while(size){
        unsigned long long n = size<CH ? size : CH;
        DSTORAGE_REQUEST r{};
        r.Options.CompressionFormat=DSTORAGE_COMPRESSION_FORMAT_NONE;
        r.Options.SourceType=DSTORAGE_REQUEST_SOURCE_FILE;
        r.Options.DestinationType=DSTORAGE_REQUEST_DESTINATION_BUFFER;
        r.Source.File.Source=f;
        r.Source.File.Offset=off;
        r.Source.File.Size=(UINT32)n;
        r.Destination.Buffer.Resource=g_res[dst_off/DS_CHUNK];
        r.Destination.Buffer.Offset=dst_off%DS_CHUNK;
        r.Destination.Buffer.Size=(UINT32)n;
        r.UncompressedSize=(UINT32)n;
        g_q->EnqueueRequest(&r);
        off+=n; dst_off+=n; size-=n;
    }
    return 1;
}

DSEXPORT int coli_cuda_ds_submit_wait(unsigned timeout_ms){
    if(g_state!=1) return 0;
    g_fence->SetEventOnCompletion(++g_fval,g_ev);
    g_q->EnqueueSignal(g_fence,g_fval);
    g_q->Submit();
    if(WaitForSingleObject(g_ev,timeout_ms?timeout_ms:120000)!=WAIT_OBJECT_0){
        fprintf(stderr,"[DS] fence timeout\n"); return 0; }
    DSTORAGE_ERROR_RECORD er{};
    g_q->RetrieveErrorRecord(&er);
    if(FAILED(er.FirstFailure.HResult)){
        fprintf(stderr,"[DS] %u request(s) failed, first hr=0x%08lx\n",
            (unsigned)er.FailureCount,(unsigned long)er.FirstFailure.HResult);
        return 0;
    }
    return 1;
}
#endif /* _WIN32 */
