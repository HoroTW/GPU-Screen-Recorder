#pragma once

#include "LibraryLoader.hpp"

#include <cuda.h>
#include <dlfcn.h>
#include <stdio.h>

typedef struct CUgraphicsResource_st *CUgraphicsResource;

struct Cuda {
    CUresult CUDAAPI (*cuInit)(unsigned int Flags);
    CUresult CUDAAPI (*cuDeviceGetCount)(int *count);
    CUresult CUDAAPI (*cuDeviceGet)(CUdevice *device, int ordinal);
    CUresult CUDAAPI (*cuCtxCreate_v2)(CUcontext *pctx, unsigned int flags, CUdevice dev);
    CUresult CUDAAPI (*cuCtxPushCurrent_v2)(CUcontext ctx);
    CUresult CUDAAPI (*cuCtxPopCurrent_v2)(CUcontext *pctx);
    CUresult CUDAAPI (*cuGetErrorString)(CUresult error, const char **pStr);
    CUresult CUDAAPI (*cuMemsetD8_v2)(CUdeviceptr dstDevice, unsigned char uc, size_t N);
    CUresult CUDAAPI (*cuMemcpy2D_v2)(const CUDA_MEMCPY2D *pCopy);

    CUresult CUDAAPI (*cuGraphicsGLRegisterImage)(CUgraphicsResource *pCudaResource, unsigned int image, unsigned int target, unsigned int Flags);
    CUresult CUDAAPI (*cuGraphicsResourceSetMapFlags)(CUgraphicsResource resource, unsigned int flags);
    CUresult CUDAAPI (*cuGraphicsMapResources)(unsigned int count, CUgraphicsResource *resources, CUstream hStream);
    CUresult CUDAAPI (*cuGraphicsUnregisterResource)(CUgraphicsResource resource);
    CUresult CUDAAPI (*cuGraphicsSubResourceGetMappedArray)(CUarray *pArray, CUgraphicsResource resource, unsigned int arrayIndex, unsigned int mipLevel);

    ~Cuda() {
        if(library)
            dlclose(library);
    }

    bool load() {
        if(library)
            return true;

        dlerror(); // clear
        void *lib = dlopen("libcuda.so", RTLD_LAZY);
        if(!lib) {
            lib = dlopen("libcuda.so.1", RTLD_LAZY);
            if(!lib) {
                fprintf(stderr, "Error: failed to load libcuda.so/libcuda.so.1, error: %s\n", dlerror());
                return false;
            }
        }

        dlsym_assign required_dlsym[] = {
            { (void**)&cuInit, "cuInit" },
            { (void**)&cuDeviceGetCount, "cuDeviceGetCount" },
            { (void**)&cuDeviceGet, "cuDeviceGet" },
            { (void**)&cuCtxCreate_v2, "cuCtxCreate_v2" },
            { (void**)&cuCtxPushCurrent_v2, "cuCtxPushCurrent_v2" },
            { (void**)&cuCtxPopCurrent_v2, "cuCtxPopCurrent_v2" },
            { (void**)&cuGetErrorString, "cuGetErrorString" },
            { (void**)&cuMemsetD8_v2, "cuMemsetD8_v2" },
            { (void**)&cuMemcpy2D_v2, "cuMemcpy2D_v2" },

            { (void**)&cuGraphicsGLRegisterImage, "cuGraphicsGLRegisterImage" },
            { (void**)&cuGraphicsResourceSetMapFlags, "cuGraphicsResourceSetMapFlags" },
            { (void**)&cuGraphicsMapResources, "cuGraphicsMapResources" },
            { (void**)&cuGraphicsUnregisterResource, "cuGraphicsUnregisterResource" },
            { (void**)&cuGraphicsSubResourceGetMappedArray, "cuGraphicsSubResourceGetMappedArray" },

            { NULL, NULL }
        };

        if(dlsym_load_list(lib, required_dlsym)) {
            library = lib;
            return true;
        } else {
            fprintf(stderr, "Error: missing required symbols in libcuda.so\n");
            dlclose(lib);
            return false;
        }
    }
private:
    void *library = nullptr;
};
