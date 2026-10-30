#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Manual NVIDIA kernel/copy test using libcuda and PTX; no CUDA toolkit needed."""
import ctypes as C

PTX = b'''.version 7.0
.target sm_52
.address_size 64
.visible .entry increment(.param .u64 ptr, .param .u32 count) {
 .reg .pred p;
 .reg .u32 r<6>;
 .reg .u64 d<4>;
 ld.param.u64 d0, [ptr];
 ld.param.u32 r0, [count];
 mov.u32 r1, %tid.x;
 mov.u32 r2, %ctaid.x;
 mov.u32 r3, %ntid.x;
 mad.lo.u32 r4, r2, r3, r1;
 setp.ge.u32 p, r4, r0;
 @p bra done;
 mul.wide.u32 d1, r4, 4;
 add.u64 d2, d0, d1;
 ld.global.u32 r5, [d2];
 add.u32 r5, r5, 7;
 st.global.u32 [d2], r5;
 done: ret;
}
'''


def main():
    cuda = C.CDLL('libcuda.so.1')
    def api(name, *types):
        function = getattr(cuda, name)
        function.argtypes = types
        function.restype = C.c_int
        def call(*args):
            error = function(*args)
            if error:
                raise RuntimeError(f'{name} failed: CUDA error {error}')
        return call
    integer, pointer, size, deviceptr = C.c_int, C.c_void_p, C.c_size_t, C.c_uint64
    api('cuInit', C.c_uint)(0)
    count = integer()
    api('cuDeviceGetCount', C.POINTER(integer))(C.byref(count))
    if count.value == 0:
        raise RuntimeError('No CUDA devices')
    for ordinal in range(count.value):
        device, context, module, function, memory = integer(), pointer(), pointer(), pointer(), deviceptr()
        api('cuDeviceGet', C.POINTER(integer), integer)(C.byref(device), ordinal)
        api('cuCtxCreate_v2', C.POINTER(pointer), C.c_uint, integer)(C.byref(context), 0, device)
        allocated = False
        try:
            name = C.create_string_buffer(256)
            api('cuDeviceGetName', pointer, integer, integer)(name, len(name), device)
            n = C.c_uint(1 << 20)
            host = (C.c_uint * n.value)(*(i * 3 for i in range(n.value)))
            api('cuMemAlloc_v2', C.POINTER(deviceptr), size)(C.byref(memory), C.sizeof(host))
            allocated = True
            api('cuMemcpyHtoD_v2', deviceptr, pointer, size)(memory, host, C.sizeof(host))
            api('cuModuleLoadData', C.POINTER(pointer), pointer)(C.byref(module), C.c_char_p(PTX))
            api('cuModuleGetFunction', C.POINTER(pointer), pointer, C.c_char_p)(C.byref(function), module, b'increment')
            arguments = (pointer * 2)(C.addressof(memory), C.addressof(n))
            api('cuLaunchKernel', pointer, *([C.c_uint] * 7), pointer, pointer, pointer)(function, n.value // 256, 1, 1, 256, 1, 1, 0, None, arguments, None)
            api('cuCtxSynchronize')()
            api('cuMemcpyDtoH_v2', pointer, deviceptr, size)(host, memory, C.sizeof(host))
            for i, value in enumerate(host):
                if value != i * 3 + 7:
                    raise RuntimeError(f'GPU {ordinal} mismatch at {i}')
            print(f'PASS CUDA GPU {ordinal}: {name.value.decode()}, {n.value} results verified', flush=True)
        finally:
            if allocated:
                api('cuMemFree_v2', deviceptr)(memory)
            api('cuCtxDestroy_v2', pointer)(context)


if __name__ == '__main__':
    main()
