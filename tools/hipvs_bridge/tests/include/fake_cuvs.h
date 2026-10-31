#pragma once
/* Host-only boundary declarations: NOT an SDK, never used by build.sh.
 * API names/signatures checked against hipVS f96a663d0a4441d6f5344bd3ef552b71ae7b370b.
 * Only fields consumed by the bridge are modeled. No GPU/recall claims. */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef enum { CUVS_ERROR, CUVS_SUCCESS } cuvsError_t;
typedef uintptr_t cuvsResources_t;
typedef enum { kDLCPU=1, kDLCUDA=2 } DLDeviceType;
typedef struct { DLDeviceType device_type; int device_id; } DLDevice;
enum { kDLUInt=1, kDLFloat=2 };
typedef struct { uint8_t code, bits; uint16_t lanes; } DLDataType;
typedef struct { void *data; DLDevice device; int ndim; DLDataType dtype; int64_t *shape; int64_t *strides; uint64_t byte_offset; } DLTensor;
typedef struct DLManagedTensor { DLTensor dl_tensor; void *manager_ctx; void (*deleter)(struct DLManagedTensor*); } DLManagedTensor;
enum cuvsCagraGraphBuildAlgo { AUTO_SELECT, IVF_PQ, NN_DESCENT, ITERATIVE_CAGRA_SEARCH };
typedef struct { enum cuvsCagraGraphBuildAlgo build_algo; size_t graph_degree, intermediate_graph_degree, nn_descent_niter; } *cuvsCagraIndexParams_t;
typedef struct { size_t itopk_size, search_width; } *cuvsCagraSearchParams_t;
typedef struct { long n, dim; float *base; } *cuvsCagraIndex_t;
typedef enum { NO_FILTER } cuvsFilterType;
typedef struct { cuvsFilterType type; uintptr_t addr; } cuvsFilter;
const char *cuvsGetLastErrorText(void);
cuvsError_t cuvsResourcesCreate(cuvsResources_t*);
cuvsError_t cuvsResourcesDestroy(cuvsResources_t);
cuvsError_t cuvsRMMAlloc(cuvsResources_t,void**,size_t);
cuvsError_t cuvsRMMFree(cuvsResources_t,void*,size_t);
cuvsError_t cuvsCagraIndexParamsCreate(cuvsCagraIndexParams_t*);
cuvsError_t cuvsCagraIndexParamsDestroy(cuvsCagraIndexParams_t);
cuvsError_t cuvsCagraSearchParamsCreate(cuvsCagraSearchParams_t*);
cuvsError_t cuvsCagraSearchParamsDestroy(cuvsCagraSearchParams_t);
cuvsError_t cuvsCagraIndexCreate(cuvsCagraIndex_t*);
cuvsError_t cuvsCagraIndexDestroy(cuvsCagraIndex_t);
cuvsError_t cuvsCagraBuild(cuvsResources_t,cuvsCagraIndexParams_t,DLManagedTensor*,cuvsCagraIndex_t);
cuvsError_t cuvsCagraSearch(cuvsResources_t,cuvsCagraSearchParams_t,cuvsCagraIndex_t,DLManagedTensor*,DLManagedTensor*,DLManagedTensor*,cuvsFilter);
cuvsError_t cuvsCagraSerialize(cuvsResources_t,const char*,cuvsCagraIndex_t,bool);
cuvsError_t cuvsCagraDeserialize(cuvsResources_t,const char*,cuvsCagraIndex_t);
cuvsError_t cuvsCagraIndexGetSize(cuvsCagraIndex_t,int64_t*);
cuvsError_t cuvsCagraIndexGetDims(cuvsCagraIndex_t,int64_t*);
typedef enum { cudaSuccess=0, cudaErrorUnknown=1 } cudaError_t;
typedef enum { cudaMemcpyDefault=4 } cudaMemcpyKind;
cudaError_t cudaMemcpy(void*,const void*,size_t,cudaMemcpyKind);
#ifdef __cplusplus
}
#endif
