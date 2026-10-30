// SPDX-License-Identifier: GPL-2.0-only
/** @file hip_smoke.cpp
 * @brief Manual HIP kernel/copy validation, separate from CPU-only CI.
 */
#include <hip/hip_runtime.h>
#include <cstdio>
#include <cstdlib>
#include <vector>
/** @brief Check a HIP call and stop immediately if the runtime reports failure.
 * @param call HIP runtime expression returning hipError_t.
 */
#define HIP(call)                                                                                  \
    do {                                                                                           \
        hipError_t e = (call);                                                                     \
        if (e != hipSuccess) {                                                                     \
            std::fprintf(stderr, "%s: %s\n", #call, hipGetErrorString(e));                         \
            std::exit(1);                                                                          \
        }                                                                                          \
    } while (0)
/**
 * @brief Add two GPU arrays, guarding the final partial thread block.
 * @param a First device input array.
 * @param b Second device input array.
 * @param c Device output array.
 * @param n Number of elements in each array.
 */
__global__ void add(const int *a, const int *b, int *c, unsigned n)
{
    unsigned i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n)
        c[i] = a[i] + b[i];
}
/** @brief Verify copies and one kernel on every detected HIP GPU.
 * @return Zero if every result matches; nonzero on runtime or data failure.
 */
int main()
{
    int devices;
    HIP(hipGetDeviceCount(&devices));
    if (!devices)
        return 1;
    constexpr unsigned n = 1U << 20;
    const size_t bytes = n * sizeof(int);
    std::vector<int> a(n), b(n), c(n);
    for (unsigned i = 0; i < n; ++i) {
        a[i] = static_cast<int>(i);
        b[i] = static_cast<int>(3 * i + 7);
    }
    for (int device = 0; device < devices; ++device) {
        HIP(hipSetDevice(device));
        hipDeviceProp_t props;
        HIP(hipGetDeviceProperties(&props, device));
        int *da, *db, *dc;
        HIP(hipMalloc(&da, bytes));
        HIP(hipMalloc(&db, bytes));
        HIP(hipMalloc(&dc, bytes));
        HIP(hipMemcpy(da, a.data(), bytes, hipMemcpyHostToDevice));
        HIP(hipMemcpy(db, b.data(), bytes, hipMemcpyHostToDevice));
        hipLaunchKernelGGL(add, dim3((n + 255) / 256), dim3(256), 0, 0, da, db, dc, n);
        HIP(hipGetLastError());
        HIP(hipDeviceSynchronize());
        HIP(hipMemcpy(c.data(), dc, bytes, hipMemcpyDeviceToHost));
        for (unsigned i = 0; i < n; ++i)
            if (c[i] != a[i] + b[i]) {
                std::fprintf(stderr, "GPU %d mismatch at %u\n", device, i);
                return 1;
            }
        HIP(hipFree(dc));
        HIP(hipFree(db));
        HIP(hipFree(da));
        std::printf("PASS HIP GPU %d: %s (%s), %u results verified\n", device, props.name,
                    props.gcnArchName, n);
        std::fflush(stdout);
    }
}
