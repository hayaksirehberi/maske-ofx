// Metal backend for macOS Resolve. NOT yet tested — written on Windows;
// compile and verify on a Mac (see README).
#ifndef HB_HAS_METAL
#define HB_HAS_METAL
#endif
#include "GpuBackend.h"

#import <Metal/Metal.h>
#import <Foundation/Foundation.h>

#include <mutex>
#include <map>

static const char* kKernelSource = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct GpuStamp {
    float cx, cy;
    float rx, ry;
    float inner;
    int   erase;
};

struct Params {
    int srcRowElems;
    int srcX1, srcY1, srcX2, srcY2;
    int dstRowElems;
    int dstX1, dstY1;
    int winX1, winY1, winW, winH;
    int nStamps;
    int invert;
    int showMatte;
    int hasSrc;
};

kernel void brushKernel(const device float* src [[buffer(0)]],
                        device float* dst [[buffer(1)]],
                        const device GpuStamp* stamps [[buffer(2)]],
                        constant Params& p [[buffer(3)]],
                        uint2 gid [[thread_position_in_grid]]) {
    if (gid.x >= (uint)p.winW || gid.y >= (uint)p.winH) return;
    int x = p.winX1 + (int)gid.x;
    int y = p.winY1 + (int)gid.y;

    float a = 0.0f;
    float px = x + 0.5f, py = y + 0.5f;
    for (int i = 0; i < p.nStamps; ++i) {
        GpuStamp s = stamps[i];
        float nx = (px - s.cx) / s.rx;
        float ny = (py - s.cy) / s.ry;
        float d2 = nx * nx + ny * ny;
        if (d2 >= 1.0f) continue;
        float d = sqrt(d2);
        float c;
        if (d <= s.inner) c = 1.0f;
        else {
            c = (1.0f - d) / (1.0f - s.inner);
            c = c * c * (3.0f - 2.0f * c);
        }
        if (s.erase != 0) a *= (1.0f - c);
        else              a = max(a, c);
    }
    if (p.invert != 0) a = 1.0f - a;

    float r = 0.0f, g = 0.0f, b = 0.0f;
    if (p.hasSrc != 0 && x >= p.srcX1 && x < p.srcX2 && y >= p.srcY1 && y < p.srcY2) {
        ulong idx = (ulong)(y - p.srcY1) * (ulong)p.srcRowElems + (ulong)(x - p.srcX1) * 4;
        r = src[idx]; g = src[idx + 1]; b = src[idx + 2];
    }
    if (p.showMatte != 0) { r = g = b = a; }

    ulong odx = (ulong)(y - p.dstY1) * (ulong)p.dstRowElems + (ulong)(x - p.dstX1) * 4;
    dst[odx] = r; dst[odx + 1] = g; dst[odx + 2] = b; dst[odx + 3] = a;
}
)MSL";

struct MetalParams {
    int srcRowElems;
    int srcX1, srcY1, srcX2, srcY2;
    int dstRowElems;
    int dstX1, dstY1;
    int winX1, winY1, winW, winH;
    int nStamps;
    int invert;
    int showMatte;
    int hasSrc;
};

// One compiled pipeline per device, built lazily.
static std::mutex gPipelineMutex;
static std::map<void*, id<MTLComputePipelineState>> gPipelines;

static id<MTLComputePipelineState> pipelineForDevice(id<MTLDevice> device) {
    std::lock_guard<std::mutex> lock(gPipelineMutex);
    auto it = gPipelines.find((__bridge void*)device);
    if (it != gPipelines.end()) return it->second;

    NSError* err = nil;
    id<MTLLibrary> lib = [device newLibraryWithSource:[NSString stringWithUTF8String:kKernelSource]
                                              options:nil
                                                error:&err];
    if (!lib) return nil;
    id<MTLFunction> fn = [lib newFunctionWithName:@"brushKernel"];
    if (!fn) return nil;
    id<MTLComputePipelineState> pso = [device newComputePipelineStateWithFunction:fn error:&err];
    if (!pso) return nil;
    gPipelines[(__bridge void*)device] = pso;
    return pso;
}

extern "C" bool HistoryBrushMetalRender(void* queuePtr, const GpuRenderArgs* args) {
    @autoreleasepool {
        id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)queuePtr;
        if (!queue) return false;
        id<MTLDevice> device = queue.device;
        id<MTLComputePipelineState> pso = pipelineForDevice(device);
        if (!pso) return false;

        id<MTLBuffer> srcBuf = args->src ? (__bridge id<MTLBuffer>)(void*)args->src : nil;
        id<MTLBuffer> dstBuf = (__bridge id<MTLBuffer>)args->dst;
        if (!dstBuf) return false;

        id<MTLBuffer> stampBuf = nil;
        if (args->nStamps > 0) {
            stampBuf = [device newBufferWithBytes:args->stamps
                                           length:args->nStamps * sizeof(GpuStamp)
                                          options:MTLResourceStorageModeManaged];
        } else {
            // Metal requires a bound buffer even when unused.
            GpuStamp dummy = {};
            stampBuf = [device newBufferWithBytes:&dummy length:sizeof(GpuStamp)
                                          options:MTLResourceStorageModeManaged];
        }

        MetalParams p;
        p.srcRowElems = args->srcRowElems;
        p.srcX1 = args->srcX1; p.srcY1 = args->srcY1;
        p.srcX2 = args->srcX2; p.srcY2 = args->srcY2;
        p.dstRowElems = args->dstRowElems;
        p.dstX1 = args->dstX1; p.dstY1 = args->dstY1;
        p.winX1 = args->winX1; p.winY1 = args->winY1;
        p.winW = args->winX2 - args->winX1;
        p.winH = args->winY2 - args->winY1;
        p.nStamps = args->nStamps;
        p.invert = args->invert;
        p.showMatte = args->showMatte;
        p.hasSrc = srcBuf ? 1 : 0;

        id<MTLCommandBuffer> cmd = [queue commandBuffer];
        id<MTLComputeCommandEncoder> enc = [cmd computeCommandEncoder];
        [enc setComputePipelineState:pso];
        [enc setBuffer:(srcBuf ? srcBuf : dstBuf) offset:0 atIndex:0];
        [enc setBuffer:dstBuf offset:0 atIndex:1];
        [enc setBuffer:stampBuf offset:0 atIndex:2];
        [enc setBytes:&p length:sizeof(p) atIndex:3];

        MTLSize tg = MTLSizeMake(16, 16, 1);
        MTLSize grid = MTLSizeMake(((NSUInteger)p.winW + 15) / 16,
                                   ((NSUInteger)p.winH + 15) / 16, 1);
        [enc dispatchThreadgroups:grid threadsPerThreadgroup:tg];
        [enc endEncoding];
        [cmd commit];
        // Same command queue as the host: queue ordering keeps downstream
        // nodes correct without a blocking wait.
        return true;
    }
}
