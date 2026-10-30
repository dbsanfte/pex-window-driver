# Hardware validation — 2026-10-30

Validated live on a Supermicro X11DPi-N (BIOS 4.7), Ubuntu kernel 6.14.0-37-generic, Secure Boot disabled. AMD DKMS 7.1.9, ROCm 7.1.1, NVIDIA 580.126.09. Two top-level PEX88096 GPU trees, with nested switches, share neither their CPU root port nor their 1 TiB host MMIO aperture.

- Eight CTest cases passed with address/undefined-behavior sanitizers and assertions enabled.
- Read-only discovery found 58 devices, 50 resource changes, and five GPU slots per tree.
- Application with drivers still bound was refused before changes.
- A deliberate failure after programming verified exact restoration of all original resource ranges/flags and relevant PCI registers. All seven GPUs rebound afterward.
- Application succeeded: each tree reserves 320 GiB; leaf GPU paths reserve 64 GiB; two-slot branches reserve 128 GiB.
- All seven GPUs have 32 GiB VRAM BARs after rebinding. The RX 7900 XTX and both RTX 3090s grew from 256 MiB; the four MI50s retained their 32 GiB size and moved to the new layout.
- NVIDIA reports 32768 MiB BAR1 on both RTX 3090s. AMD reports BAR=32768M on all five cards, including the 24 GiB RX 7900 XTX.
- HIP kernel/copy tests passed on all five AMD GPUs and CUDA kernel/copy tests passed on both NVIDIA GPUs, verifying 1,048,576 results per GPU.

Live rollback/application are tested. Boot ordering requires the next reboot and is not claimed as tested. Long-running workloads, hotplug, other PEX models, other motherboards, and Secure Boot key enrollment have not been hardware-tested. CI builds provide compilation coverage, not hardware validation.
