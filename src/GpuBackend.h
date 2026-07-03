// Shared interface between the OFX plugin and the per-platform GPU backends.
#pragma once

// Stamp precomputed into output pixel space (render scale + PAR applied).
struct GpuStamp {
    float cx, cy;    // center
    float rx, ry;    // radii
    float inner;     // 1 - softness, hard-core fraction of the radius
    int   erase;     // 0 paint, 1 erase
};

struct GpuRenderArgs {
    // src may be null (no input); pointers are device pointers (CUDA) or
    // MTLBuffer objects (Metal), always float RGBA.
    const void* src;
    int srcRowElems;                 // floats per row
    int srcX1, srcY1, srcX2, srcY2;  // bounds
    void* dst;
    int dstRowElems;
    int dstX1, dstY1;
    int winX1, winY1, winX2, winY2;  // render window
    const GpuStamp* stamps;          // host memory
    int nStamps;
    int invert;
    int showMatte;
};

#ifdef HB_HAS_CUDA
// stream is the host-provided cudaStream_t. Returns false on CUDA error.
extern "C" bool HistoryBrushCudaRender(void* stream, const GpuRenderArgs* args);
#endif

#ifdef HB_HAS_METAL
// queue is the host-provided id<MTLCommandQueue>. Returns false on failure.
extern "C" bool HistoryBrushMetalRender(void* queue, const GpuRenderArgs* args);
#endif
