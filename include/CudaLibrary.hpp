#pragma once

#include "LibraryLoader.hpp"

#include <dlfcn.h>
#include <stdio.h>

// To prevent hwcontext_cuda.h from including cuda.h
#define CUDA_VERSION 11070

#if defined(_WIN64) || defined(__LP64__)
typedef unsigned long long CUdeviceptr_v2;
#else
typedef unsigned int CUdeviceptr_v2;
#endif
typedef CUdeviceptr_v2 CUdeviceptr;

typedef int CUresult;
typedef int CUdevice_v1;
typedef CUdevice_v1 CUdevice;
typedef struct CUctx_st *CUcontext;
typedef struct CUstream_st *CUstream;
typedef struct CUarray_st *CUarray;

static const int CUDA_SUCCESS = 0;

typedef enum CUgraphicsMapResourceFlags_enum {
    CU_GRAPHICS_MAP_RESOURCE_FLAGS_NONE          = 0x00,
    CU_GRAPHICS_MAP_RESOURCE_FLAGS_READ_ONLY     = 0x01,
    CU_GRAPHICS_MAP_RESOURCE_FLAGS_WRITE_DISCARD = 0x02
} CUgraphicsMapResourceFlags;

typedef enum CUgraphicsRegisterFlags_enum {
    CU_GRAPHICS_REGISTER_FLAGS_NONE           = 0x00,
    CU_GRAPHICS_REGISTER_FLAGS_READ_ONLY      = 0x01,
    CU_GRAPHICS_REGISTER_FLAGS_WRITE_DISCARD  = 0x02,
    CU_GRAPHICS_REGISTER_FLAGS_SURFACE_LDST   = 0x04,
    CU_GRAPHICS_REGISTER_FLAGS_TEXTURE_GATHER = 0x08
} CUgraphicsRegisterFlags;

typedef enum CUmemorytype_enum {
    CU_MEMORYTYPE_HOST    = 0x01,    /**< Host memory */
    CU_MEMORYTYPE_DEVICE  = 0x02,    /**< Device memory */
    CU_MEMORYTYPE_ARRAY   = 0x03,    /**< Array memory */
    CU_MEMORYTYPE_UNIFIED = 0x04     /**< Unified device or host memory */
} CUmemorytype;

typedef struct CUDA_MEMCPY2D_st {
    size_t srcXInBytes;         /**< Source X in bytes */
    size_t srcY;                /**< Source Y */

    CUmemorytype srcMemoryType; /**< Source memory type (host, device, array) */
    const void *srcHost;        /**< Source host pointer */
    CUdeviceptr srcDevice;      /**< Source device pointer */
    CUarray srcArray;           /**< Source array reference */
    size_t srcPitch;            /**< Source pitch (ignored when src is array) */

    size_t dstXInBytes;         /**< Destination X in bytes */
    size_t dstY;                /**< Destination Y */

    CUmemorytype dstMemoryType; /**< Destination memory type (host, device, array) */
    void *dstHost;              /**< Destination host pointer */
    CUdeviceptr dstDevice;      /**< Destination device pointer */
    CUarray dstArray;           /**< Destination array reference */
    size_t dstPitch;            /**< Destination pitch (ignored when dst is array) */

    size_t WidthInBytes;        /**< Width of 2D memory copy in bytes */
    size_t Height;              /**< Height of 2D memory copy */
} CUDA_MEMCPY2D_v2;
typedef CUDA_MEMCPY2D_v2 CUDA_MEMCPY2D;

static const int CU_CTX_SCHED_AUTO = 0;

typedef struct CUgraphicsResource_st *CUgraphicsResource;

struct Cuda {
    CUresult (*cuInit)(unsigned int Flags);
    CUresult (*cuDeviceGetCount)(int *count);
    CUresult (*cuDeviceGet)(CUdevice *device, int ordinal);
    CUresult (*cuCtxCreate_v2)(CUcontext *pctx, unsigned int flags, CUdevice dev);
    CUresult (*cuCtxPushCurrent_v2)(CUcontext ctx);
    CUresult (*cuCtxPopCurrent_v2)(CUcontext *pctx);
    CUresult (*cuGetErrorString)(CUresult error, const char **pStr);
    CUresult (*cuMemsetD8_v2)(CUdeviceptr dstDevice, unsigned char uc, size_t N);
    CUresult (*cuMemcpy2D_v2)(const CUDA_MEMCPY2D *pCopy);

    CUresult (*cuGraphicsGLRegisterImage)(CUgraphicsResource *pCudaResource, unsigned int image, unsigned int target, unsigned int Flags);
    CUresult (*cuGraphicsResourceSetMapFlags)(CUgraphicsResource resource, unsigned int flags);
    CUresult (*cuGraphicsMapResources)(unsigned int count, CUgraphicsResource *resources, CUstream hStream);
    CUresult (*cuGraphicsUnregisterResource)(CUgraphicsResource resource);
    CUresult (*cuGraphicsSubResourceGetMappedArray)(CUarray *pArray, CUgraphicsResource resource, unsigned int arrayIndex, unsigned int mipLevel);

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
