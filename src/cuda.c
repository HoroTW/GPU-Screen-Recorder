#include "../include/cuda.h"
#include "../include/library_loader.h"
#include <string.h>

bool gsr_cuda_load(gsr_cuda *self) {
    memset(self, 0, sizeof(gsr_cuda));

    dlerror(); /* clear */
    void *lib = dlopen("libcuda.so.1", RTLD_LAZY);
    if(!lib) {
        lib = dlopen("libcuda.so", RTLD_LAZY);
        if(!lib) {
            fprintf(stderr, "gsr error: gsr_cuda_load failed: failed to load libcuda.so/libcuda.so.1, error: %s\n", dlerror());
            return false;
        }
    }

    dlsym_assign required_dlsym[] = {
        { (void**)&self->cuInit, "cuInit" },
        { (void**)&self->cuDeviceGetCount, "cuDeviceGetCount" },
        { (void**)&self->cuDeviceGet, "cuDeviceGet" },
        { (void**)&self->cuCtxCreate_v2, "cuCtxCreate_v2" },
        { (void**)&self->cuCtxDestroy_v2, "cuCtxDestroy_v2" },
        { (void**)&self->cuCtxPushCurrent_v2, "cuCtxPushCurrent_v2" },
        { (void**)&self->cuCtxPopCurrent_v2, "cuCtxPopCurrent_v2" },
        { (void**)&self->cuGetErrorString, "cuGetErrorString" },
        { (void**)&self->cuMemsetD8_v2, "cuMemsetD8_v2" },
        { (void**)&self->cuMemcpy2D_v2, "cuMemcpy2D_v2" },

        { (void**)&self->cuGraphicsGLRegisterImage, "cuGraphicsGLRegisterImage" },
        { (void**)&self->cuGraphicsResourceSetMapFlags, "cuGraphicsResourceSetMapFlags" },
        { (void**)&self->cuGraphicsMapResources, "cuGraphicsMapResources" },
        { (void**)&self->cuGraphicsUnmapResources, "cuGraphicsUnmapResources" },
        { (void**)&self->cuGraphicsUnregisterResource, "cuGraphicsUnregisterResource" },
        { (void**)&self->cuGraphicsSubResourceGetMappedArray, "cuGraphicsSubResourceGetMappedArray" },

        { NULL, NULL }
    };

    if(!dlsym_load_list(lib, required_dlsym)) {
        fprintf(stderr, "gsr error: gsr_cuda_load failed: missing required symbols in libcuda.so/libcuda.so.1\n");
        dlclose(lib);
        memset(self, 0, sizeof(gsr_cuda));
        return false;
    }

    CUresult res;

    res = self->cuInit(0);
    if(res != CUDA_SUCCESS) {
        const char *err_str = "unknown";
        self->cuGetErrorString(res, &err_str);
        fprintf(stderr, "gsr error: gsr_cuda_load failed: cuInit failed, error: %s (result: %d)\n", err_str, res);
        goto fail;
    }

    int nGpu = 0;
    self->cuDeviceGetCount(&nGpu);
    if(nGpu <= 0) {
        fprintf(stderr, "gsr error: gsr_cuda_load failed: no cuda supported devices found\n");
        goto fail;
    }

    CUdevice cu_dev;
    res = self->cuDeviceGet(&cu_dev, 0);
    if(res != CUDA_SUCCESS) {
        const char *err_str = "unknown";
        self->cuGetErrorString(res, &err_str);
        fprintf(stderr, "gsr error: gsr_cuda_load failed: unable to get CUDA device, error: %s (result: %d)\n", err_str, res);
        goto fail;
    }

    res = self->cuCtxCreate_v2(&self->cu_ctx, CU_CTX_SCHED_AUTO, cu_dev);
    if(res != CUDA_SUCCESS) {
        const char *err_str = "unknown";
        self->cuGetErrorString(res, &err_str);
        fprintf(stderr, "gsr error: gsr_cuda_load failed: unable to create CUDA context, error: %s (result: %d)\n", err_str, res);
        goto fail;
    }

    self->library = lib;
    return true;

    fail:
    dlclose(lib);
    memset(self, 0, sizeof(gsr_cuda));
    return false;
}

void gsr_cuda_unload(gsr_cuda *self) {
    if(self->library) {
        if(self->cu_ctx) {
            self->cuCtxDestroy_v2(self->cu_ctx);
            self->cu_ctx = 0;
        }

        dlclose(self->library);
        memset(self, 0, sizeof(gsr_cuda));
    }
}
