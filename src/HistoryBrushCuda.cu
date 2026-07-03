#ifndef HB_HAS_CUDA
#define HB_HAS_CUDA
#endif
#include "GpuBackend.h"
#include <cuda_runtime.h>

__global__ static void brushKernel(const float* src, int srcRowElems,
                                   int srcX1, int srcY1, int srcX2, int srcY2,
                                   float* dst, int dstRowElems, int dstX1, int dstY1,
                                   int winX1, int winY1, int winW, int winH,
                                   const GpuStamp* stamps, int nStamps,
                                   int invert, int showMatte) {
    int ix = blockIdx.x * blockDim.x + threadIdx.x;
    int iy = blockIdx.y * blockDim.y + threadIdx.y;
    if (ix >= winW || iy >= winH) return;
    int x = winX1 + ix;
    int y = winY1 + iy;

    float a = 0.0f;
    float px = x + 0.5f, py = y + 0.5f;
    for (int i = 0; i < nStamps; ++i) {
        GpuStamp s = stamps[i];
        float nx = (px - s.cx) / s.rx;
        float ny = (py - s.cy) / s.ry;
        float d2 = nx * nx + ny * ny;
        if (d2 >= 1.0f) continue;
        float d = sqrtf(d2);
        float c;
        if (d <= s.inner) c = 1.0f;
        else {
            c = (1.0f - d) / (1.0f - s.inner);
            c = c * c * (3.0f - 2.0f * c);
        }
        if (s.erase) a *= (1.0f - c);
        else         a = fmaxf(a, c);
    }
    if (invert) a = 1.0f - a;

    float r = 0.0f, g = 0.0f, b = 0.0f;
    if (src && x >= srcX1 && x < srcX2 && y >= srcY1 && y < srcY2) {
        const float* ip = src + (size_t)(y - srcY1) * srcRowElems + (size_t)(x - srcX1) * 4;
        r = ip[0]; g = ip[1]; b = ip[2];
    }
    if (showMatte) { r = g = b = a; }

    float* op = dst + (size_t)(y - dstY1) * dstRowElems + (size_t)(x - dstX1) * 4;
    op[0] = r; op[1] = g; op[2] = b; op[3] = a;
}

extern "C" bool HistoryBrushCudaRender(void* stream, const GpuRenderArgs* args) {
    cudaStream_t s = (cudaStream_t)stream;
    GpuStamp* devStamps = nullptr;
    if (args->nStamps > 0) {
        if (cudaMalloc(&devStamps, args->nStamps * sizeof(GpuStamp)) != cudaSuccess)
            return false;
        if (cudaMemcpyAsync(devStamps, args->stamps, args->nStamps * sizeof(GpuStamp),
                            cudaMemcpyHostToDevice, s) != cudaSuccess) {
            cudaFree(devStamps);
            return false;
        }
    }

    int winW = args->winX2 - args->winX1;
    int winH = args->winY2 - args->winY1;
    dim3 block(16, 16);
    dim3 grid((winW + block.x - 1) / block.x, (winH + block.y - 1) / block.y);

    brushKernel<<<grid, block, 0, s>>>(
        (const float*)args->src, args->srcRowElems,
        args->srcX1, args->srcY1, args->srcX2, args->srcY2,
        (float*)args->dst, args->dstRowElems, args->dstX1, args->dstY1,
        args->winX1, args->winY1, winW, winH,
        devStamps, args->nStamps, args->invert, args->showMatte);

    bool ok = (cudaGetLastError() == cudaSuccess);
    if (devStamps) {
        // The stamp buffer must outlive the kernel; sync before freeing.
        cudaStreamSynchronize(s);
        cudaFree(devStamps);
    }
    return ok;
}
