// Standalone smoke test for the CUDA render path — runs without Resolve.
#ifndef HB_HAS_CUDA
#define HB_HAS_CUDA
#endif
#include "../src/GpuBackend.h"
#include <cuda_runtime.h>
#include <cstdio>
#include <cmath>
#include <vector>

#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); return 1; } } while (0)

int main() {
    const int W = 64, H = 64;
    std::vector<float> hostSrc(W * H * 4);
    for (int i = 0; i < W * H; ++i) {
        hostSrc[i * 4 + 0] = 0.25f;
        hostSrc[i * 4 + 1] = 0.50f;
        hostSrc[i * 4 + 2] = 0.75f;
        hostSrc[i * 4 + 3] = 1.00f;
    }

    float *devSrc = nullptr, *devDst = nullptr;
    size_t bytes = hostSrc.size() * sizeof(float);
    CHECK(cudaMalloc(&devSrc, bytes) == cudaSuccess, "cudaMalloc src");
    CHECK(cudaMalloc(&devDst, bytes) == cudaSuccess, "cudaMalloc dst");
    CHECK(cudaMemcpy(devSrc, hostSrc.data(), bytes, cudaMemcpyHostToDevice) == cudaSuccess, "H2D");

    // One hard-edged paint stamp in the center, radius 10
    GpuStamp stamp = { W / 2.0f, H / 2.0f, 10.0f, 10.0f, 0.99f, 0 };

    GpuRenderArgs ga = {};
    ga.src = devSrc; ga.srcRowElems = W * 4;
    ga.srcX1 = 0; ga.srcY1 = 0; ga.srcX2 = W; ga.srcY2 = H;
    ga.dst = devDst; ga.dstRowElems = W * 4;
    ga.dstX1 = 0; ga.dstY1 = 0;
    ga.winX1 = 0; ga.winY1 = 0; ga.winX2 = W; ga.winY2 = H;
    ga.stamps = &stamp; ga.nStamps = 1;
    ga.invert = 0; ga.showMatte = 0;

    CHECK(HistoryBrushCudaRender(nullptr, &ga), "kernel launch");
    CHECK(cudaDeviceSynchronize() == cudaSuccess, "sync");

    std::vector<float> hostDst(hostSrc.size());
    CHECK(cudaMemcpy(hostDst.data(), devDst, bytes, cudaMemcpyDeviceToHost) == cudaSuccess, "D2H");

    auto px = [&](int x, int y) { return &hostDst[(size_t)(y * W + x) * 4]; };

    CHECK(fabsf(px(32, 32)[3] - 1.0f) < 1e-4f, "center alpha should be 1");
    CHECK(px(0, 0)[3] < 1e-6f, "corner alpha should be 0");
    CHECK(fabsf(px(0, 0)[0] - 0.25f) < 1e-6f && fabsf(px(32, 32)[2] - 0.75f) < 1e-6f, "RGB passthrough");

    // Invert test
    ga.invert = 1;
    CHECK(HistoryBrushCudaRender(nullptr, &ga), "kernel launch (invert)");
    CHECK(cudaDeviceSynchronize() == cudaSuccess, "sync (invert)");
    CHECK(cudaMemcpy(hostDst.data(), devDst, bytes, cudaMemcpyDeviceToHost) == cudaSuccess, "D2H (invert)");
    CHECK(px(32, 32)[3] < 1e-4f, "inverted center alpha should be 0");
    CHECK(fabsf(px(0, 0)[3] - 1.0f) < 1e-6f, "inverted corner alpha should be 1");

    printf("PASS: CUDA render path OK\n");
    cudaFree(devSrc); cudaFree(devDst);
    return 0;
}
