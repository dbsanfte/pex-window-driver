# pex-window

**Give GPUs behind Broadcom/PLX PEX switches enough PCIe address space for large VRAM BARs.** `pex-window` is an out-of-tree Linux kernel module that plans the complete prefetchable-memory hierarchy, enlarges the bridge windows, and resizes supported GPU BARs before the GPU drivers probe.

It updates Linux's resource bookkeeping and the hardware registers together. The default policy reserves **64 GiB per GPU slot** for a **32 GiB VRAM BAR** plus auxiliary BARs and alignment. It discovers the actual topology and has no fixed switch-count or five-GPU limit.

On the development host, this enabled 32 GiB BARs on an RX 7900 XTX and two RTX 3090s while preserving the four MI50s' existing 32 GiB BAR sizes. All seven GPUs passed compute/copy tests. See [hardware validation](docs/validation.md) for the tested configuration and limits.

- [Why this module exists](#why-this-module-exists)
- [Choose a build for your kernel](#choose-a-build-for-your-kernel)
- [Build, inspect, and test](#build-inspect-and-test)
- [Install for boot](#install-for-boot)
- [Verify the result](#verify-the-result)
- [Configuration and capacity](#configuration-and-capacity)
- [Logs and debug mode](#logs-and-debug-mode)
- [Rollback, upgrades, and recovery](#rollback-upgrades-and-recovery)
- [Tests, documentation, and releases](#tests-documentation-and-releases)

## Why this module exists

A GPU can advertise a 32 GiB Resizable BAR while firmware initially assigns only 256 MiB. Every bridge between that GPU and the CPU must also forward a sufficiently large address range. If sibling GPUs already occupy a shared PEX window, a later GPU-driver resize request can fail because Linux cannot enlarge the whole occupied hierarchy at that point. `pci=realloc` alone does not necessarily fix this sequence.

`pex-window` performs the allocation as one coordinated transaction:

1. Discover eligible root-port trees, GPUs, empty GPU-capable ports, and host MMIO apertures.
2. Plan every replacement range, respecting other allocations and all selected trees sharing an aperture.
3. Require detached endpoint drivers; verify that memory decoding and bus mastering are disabled, then drain pending transactions.
4. Replace the Linux resources, program ReBAR controls/BARs/bridge windows, and read back the registers.
5. Restore command registers and PCI saved state. If a step fails, attempt verified restoration of the original layout.

This is a PCI allocation helper, not a replacement GPU or `pcieport` driver. It preserves PCI bus numbers, I/O windows, and non-prefetchable MMIO. A large BAR maps VRAM into CPU address space; reserving 320 GiB of address space does **not** consume 320 GiB of system RAM or create additional VRAM. A 24 GiB card can use a 32 GiB BAR because BAR sizes are powers of two.

The module cannot create host address space that firmware has not exposed, add PCI bus numbers, enable physical hotplug, or add a ReBAR size the GPU does not support. No GPU BIOS flash is required when the desired size is already advertised.

## Choose a build for your kernel

Start with the running release and architecture:

```sh
uname -r
uname -m
test -d "/lib/modules/$(uname -r)/build"
```

Use **DKMS source** for your own kernel configuration, automatic rebuilds, or local module signing. A kernel's major version is not enough to choose a prebuilt module: the release, architecture, configuration, and symbol versions must match.

[GitHub releases](https://github.com/dbsanfte/pex-window-driver/releases) contain a `pex-window-VERSION.tar.xz` DKMS source archive and these exact x86-64 binary targets:

| Target | Kernel release / ABI | Headers and compiler |
| --- | --- | --- |
| Development workstation | `6.14.0-37-generic` | Ubuntu 24.04 HWE `6.14.0-37.37~24.04.1`, GCC 13 |
| Upstream Linux 6 | `6.19.14-061914-generic` | Ubuntu Mainline build of Linux 6.19.14, GCC 15 |
| Upstream Linux 7 | `7.3.1-070301-generic` | Ubuntu Mainline build of Linux 7.3.1, GCC 15 |

These are compatibility targets, not kernel upgrade recommendations. Mainline binaries match the listed **Ubuntu Mainline ABI**, not an arbitrary self-built kernel with the same upstream version. Build locally for any other configuration. Prebuilt modules are unsigned; Secure Boot requires local signing and an enrolled key. Never force-load a module with a mismatched vermagic or symbol CRC.

Each binary archive contains `pex_window.ko`, `kernel.config`, `kernel.Module.symvers`, and `build.json` recording the compiler, source commit, kernel release, and header checksums. Download its adjacent `.sha256` file and verify the archive with `sha256sum -c` before extracting it. Inspect a module with:

```sh
modinfo ./pex_window.ko
modinfo -F vermagic ./pex_window.ko
```

## Build, inspect, and test

### Get the source and matching headers

For development:

```sh
git clone https://github.com/dbsanfte/pex-window-driver.git
cd pex-window-driver
```

For a versioned installation, download and extract the **DKMS source asset** from a release, rather than GitHub's automatically generated source snapshot. The DKMS asset has matching release versions in `VERSION` and `dkms.conf`. With GitHub CLI:

```sh
release_tag=$(gh release view --repo dbsanfte/pex-window-driver --json tagName --jq .tagName)
release_version=${release_tag#v}
gh release download "$release_tag" --repo dbsanfte/pex-window-driver \
  --pattern "pex-window-$release_version.tar.xz" \
  --pattern "pex-window-$release_version.tar.xz.sha256"
sha256sum -c "pex-window-$release_version.tar.xz.sha256"
tar -xf "pex-window-$release_version.tar.xz"
cd "pex-window-$release_version"
```

On Ubuntu/Debian with distribution-provided kernels:

```sh
sudo apt-get update
sudo apt-get install build-essential dkms cmake python3 initramfs-tools pciutils psmisc
sudo apt-get install "linux-headers-$(uname -r)"
```

For the pinned workstation kernel, the header package is `linux-headers-6.14.0-37-generic`. For a mainline or custom kernel, obtain its matching headers/build tree from the same provider. Use the compiler expected by that kernel. The build tree must include the configuration and a valid `Module.symvers`; `make modules_prepare` alone does not generate symbol-version CRCs. See the [kernel's external-module documentation](https://docs.kernel.org/kbuild/modules.html).

```sh
make -j4                                      # running kernel's headers
# make -j4 KDIR=/path/to/kernel/build CC=gcc-15 # another prepared target

cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DENABLE_SANITIZERS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

### Inspect without changing allocation

```sh
sudo scripts/pex-windowctl plan
sudo journalctl -k -b -g pex_window
```

The module defaults to `apply=0`. A plan temporarily wakes devices to read valid configuration and releases those runtime-PM references afterward. It does not change resource addresses. Planning reports the proposed windows and aborts if any tree cannot fit.

The controller defaults to `./pex_window.ko` in the source tree. To test an extracted prebuilt module, pass `--module /absolute/path/to/pex_window.ko`. Use `--roots 0000:3a:00.0,0000:85:00.0` to restrict discovery to particular root ports. The same options apply to `plan`, `test-rollback`, and `apply`.

### Test and apply on an idle host

Stop GPU workloads and any display session using the affected cards before proceeding. Do not start jobs or reprobe devices during the transaction.

```sh
sudo scripts/pex-windowctl test-rollback
sudo scripts/pex-windowctl apply
sudo scripts/pex-windowctl status
```

The controller checks open GPU device files, records bindings/power policy/driver overrides, guards against reprobe, and detaches only endpoints in the selected trees. The module independently refuses bound endpoint drivers. After successful validation, the controller restores the original bindings and policy.

`test-rollback` deliberately fails **after hardware programming**, verifies every original resource and relevant BAR/window/ReBAR register, then rebinds the drivers. JSON evidence goes under `evidence/`, which is excluded from Git. A successful `apply` leaves the module loaded. The controller refuses a second transaction while it is loaded; unload only when you intend to discard its saved rollback context.

## Install for boot

The supplied installer supports **DKMS plus initramfs-tools on Ubuntu/Debian**. It builds for the running kernel by default:

```sh
sudo scripts/install
# sudo scripts/install 6.14.0-37-generic  # explicit already-installed target kernel
```

It installs under `/usr/src/pex-window-VERSION`, registers DKMS automatic rebuilds, backs up the existing initramfs, and installs:

| File | Purpose |
| --- | --- |
| `/etc/modprobe.d/pex-window.conf` | Apply/capacity policy and GPU/audio soft dependencies |
| `/etc/initramfs-tools/hooks/pex-window` | Include the module in generated initramfs images |
| `/etc/initramfs-tools/scripts/init-top/00-pex-window` | Load before udev coldplug and endpoint-driver probing |
| `/var/lib/pex-window/install-state` | Installed version, target kernel, and original-image backup path |

The installer publishes its state only after building and checking the boot image. Failed stages remove this attempt's additions and restore the original initramfs; incomplete cleanup is reported explicitly. The installer does not itself reboot or change live PCI allocations.

On Secure Boot systems, enroll the DKMS signing key according to your distribution before trying to load the module. On systems using dracut or another initramfs generator, build the module with DKMS but supply equivalent **pre-udev, pre-GPU-driver loading** integration. The provided boot installer does not configure those generators. GPU drivers compiled into the kernel may bind before an initramfs module can intervene.

Inspect installation before rebooting:

```sh
dkms status -m pex-window
modinfo -F filename pex_window
modprobe --show-depends amdgpu
lsinitramfs "/boot/initrd.img-$(uname -r)" | grep -E 'pex_window|00-pex-window|pex-window.conf'
```

After saving work and stopping GPU jobs, reboot to test early allocation. A live test does not establish actual boot ordering.

## Verify the result

After boot, check the helper's outcome and the GPU drivers:

```sh
sudo journalctl -k -b -g pex_window
lspci -Dnnk
sudo lspci -s 0000:49:00.0 -vv    # replace with your GPU BDF
nvidia-smi -q -d MEMORY           # NVIDIA: BAR1 Memory Usage / Total
rocm-smi --showproductname        # AMD: enumeration/driver health
```

Look for `APPLIED`, the expected root capacities, and large GPU BARs. A 32 GiB BAR should show `size=32G` in `lspci`, or `32768 MiB` BAR1 on NVIDIA. AMD initialization logs should show `BAR=32768M`. Check bridge windows as well as endpoint BARs, and confirm the helper ran before GPU initialization.

For a functional smoke test on the development GPU families:

```sh
mkdir -p build
hipcc --offload-arch=gfx1100 --offload-arch=gfx906 -O2 tests/hip_smoke.cpp -o build/hip_smoke
./build/hip_smoke
python3 tests/cuda_smoke.py
```

The HIP test requires ROCm and architecture flags appropriate to your cards. The NVIDIA test uses `libcuda` and embedded PTX, without a CUDA toolkit. Both execute a kernel and verify 1,048,576 results per detected GPU. They establish basic operation, not long-term stability or performance.

## Configuration and capacity

The module recognizes PCIe PEX devices `1000:c010` and PLX vendor `10b5`, then follows their root-port subtree. Installed AMD/NVIDIA display functions count as GPU slots. Empty PEX downstream ports with a maximum link width of at least x8 reserve one future slot; narrow auxiliary ports are excluded. Port width is a capacity policy, not a guarantee that a connector supports a GPU or hotplug.

Every ancestor reserves the sum of its descendant slots. A five-slot example is:

```text
Root port: 320 GiB
└── PEX upstream: 320 GiB
    ├── One GPU slot: 64 GiB
    ├── Two GPU slots: 128 GiB
    └── Two GPU slots: 128 GiB
```

Arrays are sized dynamically from PCI enumeration and traversal is iterative. Any number of discovered switch trees can be planned, subject to available memory, valid topology, and each host's contiguous high-MMIO aperture. Multiple selected roots in one aperture are allocated together. Fragmentation can prevent allocation even when aggregate free bytes appear sufficient.

| Parameter | Default | Meaning |
| --- | --- | --- |
| `apply` | `0` | Plan only; installed boot configuration sets `1` |
| `roots` | automatic | Comma-separated root-port BDFs, including domain |
| `bar_gib` | `32` | Requested VRAM BAR size, power of two; existing larger BARs are retained |
| `slot_gib` | `64` | Reservation per GPU/eligible empty port, power of two |
| `empty_min_width` | `8` | Minimum maximum link width for empty-port reservations; `0` disables them |
| `debug` | `0` | Detailed debug-priority trace, writable at runtime |
| `fail_step` | `0` | Test injection: `1` after release, `2` after hardware programming |
| `rollback` | write-only | Write `1` after detaching affected endpoints to restore the saved baseline |

The requested BAR size must be supported by every card needing enlargement. `slot_gib` must fit both the VRAM and auxiliary BARs; setting it equal to `bar_gib` often leaves insufficient room. Existing larger BARs are never shrunk.

Edit `/etc/modprobe.d/pex-window.conf` for persistent policy, then regenerate the target image:

```sh
sudo update-initramfs -u -k "$(uname -r)"
```

For a live controller run, use `--bar-gib`, `--slot-gib`, `--empty-min-width`, and `--roots`. Changing boot configuration does not relocate resources in the running kernel.

The supported hardware scope is x86-64 with identity CPU/PCI MMIO addressing, assigned 64-bit root apertures, and AMD BAR0/NVIDIA BAR1 VRAM layouts. GPU audio and PEX management functions are allowed; foreign endpoints in a selected tree, bridge-local prefetchable BARs, and 32-bit-only prefetch windows are rejected. Use `roots` to exclude unrelated PEX trees. Other switch/GPU layouts need explicit support and hardware validation.

## Logs and debug mode

Normal info logs identify the module version and policy, discovered tree capacity, requested BAR resizes, and final result. Errors identify the failing phase and, where applicable, the PCI BDF, resource, address range, or configuration offset. Register mismatches include expected and observed values. Failed recovery distinguishes a verified disable from a disable that could not be verified.

Enable detailed tracing **from boot** by appending this to the kernel command line:

```text
pex_window.debug=1
```

For GRUB, append it to the existing command line in `/etc/default/grub`, preserving other parameters, run `sudo update-grub`, and reboot. It can also be set in modprobe configuration followed by initramfs regeneration. For a manual load or controller plan:

```sh
sudo insmod ./pex_window.ko apply=0 debug=1
# Or, with the module unloaded:
sudo scripts/pex-windowctl plan --debug
```

Toggle trace output while the module is loaded:

```sh
printf '1\n' | sudo tee /sys/module/pex_window/parameters/debug
cat /sys/module/pex_window/parameters/debug
printf '0\n' | sudo tee /sys/module/pex_window/parameters/debug
```

Debug mode logs discovered devices/parents, host apertures, old/new resource ranges, release/registration order, command verification, configuration writes/readback, and saved-state updates. Messages use `KERN_DEBUG`; they are available in the kernel log even when console log level hides them:

```sh
sudo journalctl -k -b -g pex_window
sudo dmesg --level=debug | grep pex_window
```

This is a one-shot allocator with an optional later rollback, not a continuously running GPU monitor. Runtime toggles affect subsequent operations; enabling debug after allocation cannot reconstruct earlier traces. Enable it before loading or at boot to capture the full transaction.

| Message / symptom | What to check |
| --- | --- |
| `detach endpoint driver ... first` | Use the idle-host controller, or load the helper earlier at boot |
| `no contiguous ... window` | Firmware high-MMIO aperture, other reservations, and `slot_gib` policy |
| `does not support ... ReBAR size mask` | Card's advertised sizes and `bar_gib` |
| `BAR... does not fit slot` | Increase `slot_gib` to include auxiliary BARs/alignment |
| `foreign endpoint` / no eligible tree | Supported topology and the `roots` filter |
| `config verify... expected... actual...` | Hardware rejected a register write; retain the complete log and evidence |
| `baseline ... restored and verified` after a failure | The transaction restored its original layout; inspect the preceding cause |
| `cannot verify ... disable` / rollback failed | Keep affected endpoints detached; recover using the saved image/reboot path |
| `invalid module format` / symbol-version error | Wrong kernel ABI, configuration, compiler expectations, or headers |

## Rollback, upgrades, and recovery

Unloading the helper preserves successful **PCI-owned** allocations but discards its saved rollback context. Reboot normally starts again from firmware allocations; installed boot integration reapplies the helper.

The advanced `rollback` parameter requires all affected endpoint drivers to be detached. It preflights current resource ownership and the availability of the original ranges. It refuses to displace new allocations, and attempts to recover the applied layout if restoring the original one fails. Use the controller's rollback test for routine validation rather than manually detaching a live GPU stack.

If the controller cannot verify restoration, it leaves affected endpoints detached, guarded against reprobe, and powered on. Its evidence JSON records the original bindings and state. Do not blindly rebind them.

To remove boot integration:

```sh
sudo scripts/uninstall
```

This removes DKMS registration, configuration and initramfs hooks, rebuilds boot images, and unloads the helper if present. Live address assignments remain until reboot. Uninstall is retryable if rebuilding an image fails; installation state is retained until removal completes. Backups under `/var/lib/pex-window/backups/` are retained.

For an upgrade, keep both release directories available, uninstall the old version, then run `sudo scripts/install` from the new version. The installer refuses to overwrite an existing installation silently.

For boot-menu recovery, append `module_blacklist=pex_window` to prevent the helper from loading. An unrelated BMC/display path is useful for recovery; GPU soft-dependency probes may fail while the helper is blacklisted. Remove the integration from a working boot and rebuild its initramfs. The original image path is recorded in `/var/lib/pex-window/install-state` until successful uninstallation.

## Tests, documentation, and releases

CTest runs the shared allocation primitives and the **same transaction engine used by the module**. Regression cases cover ignored command writes, disable-before-drain ordering, partial registration/programming failures, rollback containment, occupied original ranges, and recovery of the applied layout. Installer tests use a temporary filesystem and injected command/copy failures; they verify ownership, cleanup, delayed state publication, and uninstall retry. These tests do not emulate physical GPU hardware.

C source files, functions, parameters, return values, and transaction structures have Doxygen documentation; Python utilities have module/method docstrings. Build the API documentation with:

```sh
sudo apt-get install doxygen
cmake -S . -B build
cmake --build build --target docs
# Or: doxygen Doxyfile
```

Open `build/docs/html/index.html`. Documentation warnings fail CI. C/C++ uses clang-format 18; Python uses Black with the checked-in configuration. Assertions remain enabled in release-build unit tests, and CI enables address/undefined-behavior sanitizers.

PRs into `master` must pass **Test gate**: CTest, documentation/formatting, and all three kernel builds. Branch protection applies to administrators. Each push landing on `master` repeats the gate before publishing DKMS source, exact-ABI module archives, Doxygen HTML, and SHA-256 checksums. Header URLs/digests are pinned in [ci/kernel-targets.json](ci/kernel-targets.json). Release versions use the major/minor series in `VERSION` and the workflow run number as the patch. Publishing a release does not install it on any host.

License: [GPL-2.0-only](LICENSE).
