#ifndef COLIBRI_ARCH_H
#define COLIBRI_ARCH_H
/* Model-architecture interface: the seam that lets a model family be *selected*
 * at load time from config.json's "architectures" field instead of being
 * hardcoded into the binary. A metadata descriptor plus a registry lookup: the
 * engine reads architectures[0] (model_type as fallback), looks it up here, and
 * refuses an unknown model with a clear message rather than mis-reading its
 * weights. The metadata fields are the invariants the per-model forward-pass
 * hooks rely on. */

typedef struct ModelArch {
    const char *name;      /* exact config.json "architectures"[0] token to match */
    const char *model_type;/* config.json "model_type" fallback: HF configs always */
                           /* carry it, but "architectures" can be null (tiny oracle) */
    const char *family;    /* human-readable label for logs                       */
    int kv_compressed;     /* 1 = MLA latent KV (GLM/DeepSeek), 0 = plain MHA/GQA  */
    int has_mtp;           /* 1 = native multi-token-prediction draft head         */
    int has_dsa;           /* 1 = DeepSeek/GLM "lightning indexer" sparse attention */
} ModelArch;

/* Look up a registered architecture by a config.json token, matched against either
 * a descriptor's "architectures" name or its "model_type". Pass architectures[0] if
 * present, else model_type. Returns a static descriptor, or NULL if unsupported. */
const ModelArch *model_arch_select(const char *token);

/* Comma-joined list of supported architecture names, for error messages. */
const char *model_arch_supported(void);

#endif
