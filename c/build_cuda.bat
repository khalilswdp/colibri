@echo off
REM Build coli_cuda.dll on Windows without a preconfigured shell. Locates
REM itself, Visual Studio and CUDA instead of hardcoding paths, so it runs on
REM any machine with the toolchain installed. Equivalent to `make cuda-dll`;
REM kept for people who do not have make.
setlocal EnableDelayedExpansion
cd /d "%~dp0"

REM --- Visual Studio (nvcc needs cl.exe as its host compiler) ---------------
if not defined VSINSTALLDIR (
  set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
  if exist "!VSWHERE!" (
    for /f "usebackq tokens=*" %%i in (`"!VSWHERE!" -latest -products * ^
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 ^
        -property installationPath`) do set "VSPATH=%%i"
  )
  if not defined VSPATH (
    echo ERROR: Visual Studio with the C++ toolset was not found.
    echo        Install "Desktop development with C++", or run this from an
    echo        "x64 Native Tools Command Prompt".
    exit /b 1
  )
  call "!VSPATH!\VC\Auxiliary\Build\vcvars64.bat" >nul
)

REM --- CUDA (the installer sets CUDA_PATH system-wide) ----------------------
if not defined CUDA_PATH (
  echo ERROR: CUDA_PATH is not set - install the CUDA Toolkit or set it manually.
  exit /b 1
)
set "NVCC=%CUDA_PATH%\bin\nvcc.exe"
if not exist "%NVCC%" (
  echo ERROR: nvcc not found at "%NVCC%".
  exit /b 1
)

REM --- DirectStorage (optional): disk->VRAM DMA transport -------------------
REM Point DSTORAGE_HOME at an extracted Microsoft.Direct3D.DirectStorage
REM nuget (include/ + lib/x64). Engages only when the SDK and
REM backend_dstorage.cpp both exist; otherwise builds unchanged (the entry
REM points are optional exports).
set "DS_SRC="
set "DS_FLAGS="
set "DS_NOTE=WITHOUT DirectStorage"
if defined DSTORAGE_HOME (
  if not exist backend_dstorage.cpp (
    echo NOTE: DSTORAGE_HOME is set but this tree has no backend_dstorage.cpp - ignoring.
  ) else if exist "%DSTORAGE_HOME%\include\dstorage.h" (
    set "DS_SRC=backend_dstorage.cpp"
    set "DS_FLAGS=-I"%DSTORAGE_HOME%\include" -L"%DSTORAGE_HOME%\lib\x64" -ldstorage -ld3d12 -ldxgi"
    set "DS_NOTE=with DirectStorage"
  ) else (
    echo WARNING: DSTORAGE_HOME is set but include\dstorage.h is missing under it - ignoring.
  )
)

REM sm_120 = Blackwell; the trailing compute_120 keeps it forward-runnable via PTX.
echo building coli_cuda.dll !DS_NOTE!
"%NVCC%" -O3 -std=c++17 ^
  -gencode arch=compute_120,code=sm_120 ^
  -gencode arch=compute_120,code=compute_120 ^
  -Xcompiler=-W3 -shared -DCOLI_CUDA_BUILDING_DLL ^
  -L"%CUDA_PATH%\lib\x64" -lcudart ^
  !DS_FLAGS! ^
  backend_cuda.cu !DS_SRC! -o coli_cuda.dll
set RC=%ERRORLEVEL%
if %RC% NEQ 0 ( echo BUILD FAILED ^(exit %RC%^) ) else ( echo built coli_cuda.dll )
exit /b %RC%
