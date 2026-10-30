# pex-window

A Linux PCI allocation helper for GPU trees behind Broadcom/PLX PEX switches. It reserves large, 64-bit prefetchable bridge windows **before endpoint drivers probe**, and resizes supported AMD/NVIDIA VRAM BARs in the same transaction. It updates both Linux resource objects and PCI configuration registers.

This addresses a specific failure: firmware assigns small GPU BARs, Linux accepts those initial allocations, and a later GPU-driver ReBAR request cannot grow a shared ancestor window while sibling resources occupy it. `pci=realloc` alone does not necessarily fix that sequence. No GPU BIOS flash is required when the requested BAR size is already advertised by the card.

## Topology and capacity

There is no configured maximum number of switches, trees, GPUs, or nesting levels. Storage is allocated from the enumerated PCI device count; traversal is iterative. Limits are the PCI topology, memory available for bookkeeping, and contiguous address space in each firmware-provided host aperture. This helper cannot create address space outside those apertures or allocate additional PCI bus numbers.

By default, discovery selects root-port subtrees containing PCIe PEX devices (`1000:c010` or PLX vendor `10b5`). It follows the actual bridge hierarchy, without fixed BDFs, motherboard checks, port numbers, or a five-GPU assumption. Each installed AMD/NVIDIA display function counts as one GPU. An empty PEX downstream port with a maximum link width of at least x8 reserves one future GPU slot; narrow auxiliary ports are excluded. Empty-port reservations provide MMIO capacity, not hotplug support or bus-number reservations.

The defaults are 32 GiB per VRAM BAR and 64 GiB per GPU slot. The extra room accommodates auxiliary BARs and alignment. Each bridge receives its descendant slot count × 64 GiB; five slots need **320 GiB**, without rounding the entire tree up to 512 GiB. Multiple selected trees sharing a host aperture are planned together, excluding their old allocations while respecting other reservations and all new plans. Planning failure changes no allocation. Fragmentation can prevent allocation even when the sum of free bytes is sufficient.

| Module parameter | Default | Purpose |
| --- | --- | --- |
| `apply` | `0` | Read-only allocation plan; use `1` for a transaction |
| `roots` | automatic | Optional comma-separated root-port BDFs, including PCI domain |
| `bar_gib` | `32` | Desired VRAM BAR size, power of two; existing larger BARs are retained |
| `slot_gib` | `64` | Per-GPU/empty-port capacity, power of two |
| `empty_min_width` | `8` | Minimum maximum link width for empty-port reservations; `0` disables them |
| `fail_step` | `0` | Testing: `1` after resource release, `2` after programming and readback |
| `rollback` | write-only | Write `1` with all affected endpoint drivers detached to restore the pre-load layout |

`bar_gib` must be supported by any GPU being resized. Increase `slot_gib` when a larger GPU BAR plus auxiliary BARs will not fit. The initial scope is x86-64 systems with identity CPU/PCI MMIO addressing and assigned 64-bit root windows. GPU BAR addresses are checked against kernel resources before changes. AMD VRAM is BAR0; NVIDIA VRAM is BAR1. Other endpoint types, except their audio functions and PEX management functions, are rejected. Bridge-local prefetchable BARs and 32-bit-only prefetch windows are also rejected. Support for additional device layouts belongs in discovery/BAR policy, separate from the address allocator.

## Build and CTest

Requires a C compiler, CMake ≥3.20, make, and headers matching the target kernel. The module build is independent of CMake; CTest runs the shared, pure-C allocation helpers.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DENABLE_SANITIZERS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
make -j4                       # current kernel
# make KDIR=/path/to/headers    # another kernel
```

Tests cover GPU/auxiliary BAR packing, nested branches, overflow and alignment boundaries, 1,024 switch trees, shared-aperture allocation, exact exhaustion, and fragmentation. Assertions remain enabled in release builds. AddressSanitizer and UndefinedBehaviorSanitizer are enabled in CI.

## Inspect, test, and apply on an idle host

The module defaults to planning only. Planning resumes devices temporarily to read valid configuration, then releases the runtime-PM references. It does not change address allocation.

```sh
sudo scripts/pex-windowctl plan
sudo journalctl -k -g pex_window
sudo scripts/pex-windowctl test-rollback
sudo scripts/pex-windowctl apply
sudo scripts/pex-windowctl status
```

The controller detects open GPU device files and refuses to proceed when they are in use. It saves driver bindings, driver overrides, power policy, resources, and relevant registers; guards against automatic reprobe; detaches only endpoints in selected trees; performs the transaction; and rebinds the original drivers. The rollback test injects failure **after the new hardware configuration is written**, then compares all original resources and BAR/window/ReBAR registers before rebinding. JSON evidence is written under `evidence/` and is excluded from Git.

Do not start GPU jobs or manually reprobe devices during a live transaction. A running display or compute workload must be stopped first. The module independently requires every affected endpoint to be unbound and locks endpoint devices during allocation. Unrelated root trees, non-prefetchable BARs, bus numbers, and I/O windows are preserved.

If a transaction cannot verify restoration, the controller leaves endpoints detached with their guards in place and power on; use its recorded evidence for recovery. Do not blindly rebind them. The module refuses an explicit rollback if resource ownership or the layout has changed since allocation. Unloading the module leaves the successfully allocated, PCI-owned resources in place. Reboot returns to firmware allocation unless boot integration reapplies the helper.

## Install at boot (Ubuntu/Debian initramfs-tools + DKMS)

```sh
sudo scripts/install
```

The installer builds and installs through DKMS for the running kernel, enables automatic rebuilds for future kernels, and adds an `init-top/00-pex-window` script before udev coldplug. Soft dependencies also place the helper before `amdgpu`, `nvidia`, `nouveau`, and `snd_hda_intel`. Configuration lives in `/etc/modprobe.d/pex-window.conf`; regenerate the initramfs after changing it. Installation does not reboot the host or change its live PCI state. Secure Boot systems require enrollment of the DKMS signing key before loading the module.

An original initramfs backup is kept under `/var/lib/pex-window/backups/`, with its path recorded in `/var/lib/pex-window/install-state`. An installation failure restores that original image. For recovery at the boot menu, the kernel parameter `module_blacklist=pex_window` prevents the helper from loading; the BMC/other unrelated display remains available, while dependent GPU probes may fail until the integration is removed.

```sh
sudo scripts/uninstall
```

Uninstallation removes DKMS and boot integration and rebuilds initramfs images. It leaves current resource assignments until reboot and retains backups. A cold/warm boot test is still needed to validate actual early-probe ordering on a new platform; live tests do not establish that ordering.

## CI and releases

Pull requests into `master` run CTest and build the module against Ubuntu 24.04 generic and HWE headers. The stable **Test gate** check succeeds only if both unit tests and both module builds pass. Branch protection should require it (private-repository enforcement depends on the account's GitHub plan).

Each push to `master`, including a merged PR, runs the same gate before publishing a GitHub release. Versions use the major/minor series in `VERSION` and the release workflow run number as the patch. Assets are a version-stamped **DKMS source archive** and SHA-256 checksum; machines build their own kernel-specific module. Release publication does not remotely install drivers.

## Hardware validation

See [the validation record](docs/validation.md). Manual compute tests are intentionally separate from the CPU-only CI suite:

```sh
hipcc --offload-arch=gfx1100 --offload-arch=gfx906 -O2 tests/hip_smoke.cpp -o build/hip_smoke
./build/hip_smoke
python3 tests/cuda_smoke.py
```

The CUDA smoke test uses the installed NVIDIA driver and embedded PTX, with no toolkit dependency. Tests execute a kernel and verify 1,048,576 results on each detected GPU. They establish basic driver/compute functionality, not long-term stability or performance.

License: GPL-2.0-only.
