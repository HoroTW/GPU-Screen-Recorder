#pragma once

#include <cuda.h>
#include <cudaGL.h>
#include <dlfcn.h>
#include <stdio.h>

typedef CUresult CUDAAPI (*CUINIT)(unsigned int Flags);
typedef CUresult CUDAAPI (*CUDEVICEGETCOUNT)(int *count);
typedef CUresult CUDAAPI (*CUDEVICEGET)(CUdevice *device, int ordinal);
typedef CUresult CUDAAPI (*CUCTXCREATE_V2)(CUcontext *pctx, unsigned int flags, CUdevice dev);
typedef CUresult CUDAAPI (*CUCTXPUSHCURRENT_V2)(CUcontext ctx);
typedef CUresult CUDAAPI (*CUCTXPOPCURRENT_V2)(CUcontext *pctx);
typedef CUresult CUDAAPI (*CUGETERRORSTRING)(CUresult error, const char **pStr);
typedef CUresult CUDAAPI (*CUMEMSETD8_V2)(CUdeviceptr dstDevice, unsigned char uc, size_t N);
typedef CUresult CUDAAPI (*CUMEMCPY2D_V2)(const CUDA_MEMCPY2D *pCopy);

typedef CUresult CUDAAPI (*CUGRAPHICSGLREGISTERIMAGE)(CUgraphicsResource *pCudaResource, GLuint image, GLenum target, unsigned int Flags);
typedef CUresult CUDAAPI (*CUGRAPHICSRESOURCESETMAPFLAGS)(CUgraphicsResource resource, unsigned int flags);
typedef CUresult CUDAAPI (*CUGRAPHICSMAPRESOURCES)(unsigned int count, CUgraphicsResource *resources, CUstream hStream);
typedef CUresult CUDAAPI (*CUGRAPHICSUNREGISTERRESOURCE)(CUgraphicsResource resource);
typedef CUresult CUDAAPI (*CUGRAPHICSSUBRESOURCEGETMAPPEDARRAY)(CUarray *pArray, CUgraphicsResource resource, unsigned int arrayIndex, unsigned int mipLevel);

struct Cuda {
    CUINIT cuInit;
    CUDEVICEGETCOUNT cuDeviceGetCount;
    CUDEVICEGET cuDeviceGet;
    CUCTXCREATE_V2 cuCtxCreate_v2;
    CUCTXPUSHCURRENT_V2 cuCtxPushCurrent_v2;
    CUCTXPOPCURRENT_V2 cuCtxPopCurrent_v2;
    CUGETERRORSTRING cuGetErrorString;
    CUMEMSETD8_V2 cuMemsetD8_v2;
    CUMEMCPY2D_V2 cuMemcpy2D_v2;

    CUGRAPHICSGLREGISTERIMAGE cuGraphicsGLRegisterImage;
    CUGRAPHICSRESOURCESETMAPFLAGS cuGraphicsResourceSetMapFlags;
    CUGRAPHICSMAPRESOURCES cuGraphicsMapResources;
    CUGRAPHICSUNREGISTERRESOURCE cuGraphicsUnregisterResource;
    CUGRAPHICSSUBRESOURCEGETMAPPEDARRAY cuGraphicsSubResourceGetMappedArray;

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
                fprintf(stderr, "Error: failed to load libcuda.so and libcuda.so.1, error: %s\n", dlerror());
                return false;
            }
        }

        cuInit = (CUINIT)load_symbol(lib, "cuInit");
        if(!cuInit)
            goto fail;

        cuDeviceGetCount = (CUDEVICEGETCOUNT)load_symbol(lib, "cuDeviceGetCount");
        if(!cuDeviceGetCount)
            goto fail;

        cuDeviceGet = (CUDEVICEGET)load_symbol(lib, "cuDeviceGet");
        if(!cuDeviceGet)
            goto fail;

        cuCtxCreate_v2 = (CUCTXCREATE_V2)load_symbol(lib, "cuCtxCreate_v2");
        if(!cuCtxCreate_v2)
            goto fail;

        cuCtxPushCurrent_v2 = (CUCTXPUSHCURRENT_V2)load_symbol(lib, "cuCtxPushCurrent_v2");
        if(!cuCtxPushCurrent_v2)
            goto fail;

        cuCtxPopCurrent_v2 = (CUCTXPOPCURRENT_V2)load_symbol(lib, "cuCtxPopCurrent_v2");
        if(!cuCtxPopCurrent_v2)
            goto fail;

        cuGetErrorString = (CUGETERRORSTRING)load_symbol(lib, "cuGetErrorString");
        if(!cuGetErrorString)
            goto fail;

        cuMemsetD8_v2 = (CUMEMSETD8_V2)load_symbol(lib, "cuMemsetD8_v2");
        if(!cuMemsetD8_v2)
            goto fail;

        cuMemcpy2D_v2 = (CUMEMCPY2D_V2)load_symbol(lib, "cuMemcpy2D_v2");
        if(!cuMemcpy2D_v2)
            goto fail;

        cuGraphicsGLRegisterImage = (CUGRAPHICSGLREGISTERIMAGE)load_symbol(lib, "cuGraphicsGLRegisterImage");
        if(!cuGraphicsGLRegisterImage)
            goto fail;

        cuGraphicsResourceSetMapFlags = (CUGRAPHICSRESOURCESETMAPFLAGS)load_symbol(lib, "cuGraphicsResourceSetMapFlags");
        if(!cuGraphicsResourceSetMapFlags)
            goto fail;

        cuGraphicsMapResources = (CUGRAPHICSMAPRESOURCES)load_symbol(lib, "cuGraphicsMapResources");
        if(!cuGraphicsMapResources)
            goto fail;

        cuGraphicsUnregisterResource = (CUGRAPHICSUNREGISTERRESOURCE)load_symbol(lib, "cuGraphicsUnregisterResource");
        if(!cuGraphicsUnregisterResource)
            goto fail;

        cuGraphicsSubResourceGetMappedArray = (CUGRAPHICSSUBRESOURCEGETMAPPEDARRAY)load_symbol(lib, "cuGraphicsSubResourceGetMappedArray");
        if(!cuGraphicsSubResourceGetMappedArray)
            goto fail;

        library = lib;
        return true;

        fail:
        dlclose(lib);
        return false;
    }
private:
    void* load_symbol(void *library, const char *symbol) {
        void *sym = dlsym(library, symbol);
        if(!sym)
            fprintf(stderr, "Error: missing required symbol %s from libcuda.so\n", symbol);
        return sym;
    }
private:
    void *library = nullptr;
};
