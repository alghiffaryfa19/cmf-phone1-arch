# Arch Linux ARM + Phosh on the CMF Phone 1

Arch Linux ARM with the Phosh mobile shell on the **CMF Phone 1** by Nothing
(codename **Tetris**, MediaTek Dimensity 7300 / MT6878). It runs on Nothing's own
Android kernel (6.1) with the MediaTek vendor drivers. The Android hardware
abstraction layers for GPU, modem and camera run in a small Halium container and are
used through [libhybris](https://github.com/libhybris/libhybris).

> **Status: early preview, for people who know fastboot and Linux.** Installing wipes
> the phone's data. Standby does not work yet, so battery life is short. Read
> [Risks](#risks) before you start.

## What works

| Area | Status | Notes |
|---|---|---|
| Boot, display, touch | ✅ | Nothing kernel 6.1 + vendor modules, Arch on `userdata`, Android stays on slot a |
| Phosh on the GPU | ✅ | Mali-G615 through our wlroots **hwcomposer backend** (libhybris); software fallback built in |
| GPU apps | ✅ | GTK apps render on the Mali GPU via libhybris EGL (`android_wlegl`) |
| Auto-rotation | ✅ | Touch follows the rotation |
| Sensors | ✅ | Accelerometer, light, proximity: read straight from the SCP sensor hub (`tetris-sensors`, drop-in for iio-sensor-proxy) |
| WLAN | ✅ | MediaTek gen4m driver |
| Bluetooth | ✅ | |
| Battery / charging | ✅ | |
| Audio (speaker, mic) | ✅ | UCM config; headphone jack detection should now work, but it is not tested |
| Vibration | ✅ | feedbackd through a force-feedback bridge (`tetris-vibra`) |
| Torch | ✅ | Phosh quick setting (`tetris-torch`, flash LED via uleds) |
| Cameras | ✅ | Main (50 MP), front, and the rear IR depth sensor; own app **Kamera** with an IR false-colour mode |
| Mobile data (LTE) | ✅ | ModemManager with our plugin for the MediaTek modem; IPv4 only for now |
| SIM PIN, SMS receive | ✅ | Chats (Chatty) |
| SMS send | ❓ | Not tested |
| Calls | ❓ | GNOME Calls is installed but not tested yet. No VoLTE (needs MediaTek's IMS stack), so calls need a 2G/3G fallback |
| Standby (suspend) | ❌ | The SoC hangs in deep sleep. The screen can still turn off, but the CPU stays awake |
| Fingerprint, NFC, GPS | ❌ | Not started |

## How it is put together

- **Kernel:** Nothing's open source kernel and MediaTek vendor modules
  ([NothingOSS](https://github.com/NothingOSS)), built with Android's clang and plain
  `make`. There are a few small patches in `patches/kernel*`.
- **Boot:** The kernel goes to `boot_b`. A small busybox initramfs goes into
  `vendor_boot_b`; it loads the vendor modules in stock order and mounts the Arch rootfs
  from `userdata`. Slot a keeps Nothing OS. The bootloader has no `fastboot boot`, so
  every test means flashing slot b.
- **Halium container (LXC):** a LuneOS Halium 16 GSI runs the vendor HALs: gralloc,
  composer, modem (`ccci_mdinit`), camera, power and codec2. It runs in its own network
  namespace and cannot suspend the host.
- **GPU:** a wlroots 0.20 **hwcomposer backend**, a gralloc allocator and
  `android_wlegl` client buffers (`patches/wlroots/`). phoc composites on the Mali GPU
  and hands frames to Android's composer HAL.
- **Modem:** a tty bridge kernel module (`tetris_mdtty`) and a ModemManager plugin for
  MediaTek's AT and data interface (`patches/modemmanager/tetris`).
- **Own services** (`rootfs/overlay`, `rootfs/src`): Phosh session with GPU/software
  selection and crash guard, `tetris-sensors`, `tetris-vibra`, `tetris-torch`, the
  camera stack (`tetris-camera`) and the **Kamera** app (Camera2 NDK via libhybris).

## Building and installing

You need an x86_64 Linux PC (developed on Arch Linux) and the phone with an
**unlocked bootloader**.

### Inputs you have to get yourself

These are not in the repository: they are proprietary, or too large to include.

| Path | What |
|---|---|
| `firmware/Tetris_B4.1-260928-1140/images/` | Stock Nothing OS **B4.1** fastboot images (`vendor_boot.img`, `vendor.img`, ...) for Tetris |
| `toolchain/clang-r487747c/` | Android clang **r487747c** (AOSP `prebuilts/clang/host/linux-x86`), the same version Nothing uses |
| `downloads/alarm/` | `ArchLinuxARM-aarch64-latest.tar.gz` and the static `busybox` from Arch Linux ARM's `busybox` package (unpack it so that `downloads/alarm/usr/bin/busybox` exists) |
| Halium image | LuneOS `halium-luneos-16.0-20261017-1-halium_arm64` from [webOS-ports/halium-images](https://github.com/webOS-ports/halium-images); you need the unpacked `system.img` |

Host packages: `android-tools` (fastboot, mkbootimg), `arch-install-scripts`,
`qemu-user-static-binfmt`, `erofs-utils`, `lz4`, `cpio`, `e2fsprogs`, `git`, `curl`.

### Steps

```sh
scripts/fetch-sources.sh          # Nothing kernel sources + our patches into src/
scripts/build-kernel.sh           # kernel, vendor modules, out-of-tree modules
scripts/build-debug-boot.sh       # boot_b / vendor_boot_b / init_boot_b images
sudo scripts/build-rootfs.sh      # out/rootfs.simg (Arch Linux ARM + overlay)
```

Then flash, with the phone in fastboot (Power + Volume down):

```sh
# Optional, from a clean state: stock B4.1 on both slots, AVB off (needed for our kernel)
scripts/flash-stock.sh firmware/Tetris_B4.1-260928-1140/images
fastboot flash userdata out/rootfs.simg     # wipes all Android data
scripts/flash-debug-b.sh                    # our kernel + initramfs to slot b, boot it
```

After the first boot, connect USB (the phone is `172.16.42.1`) and run the second stage.
It builds libhybris, wlroots and the ModemManager plugin on the phone and installs the
Halium image:

```sh
scripts/setup-phone.sh /path/to/halium/system.img
```

The user is `alarm` with password `alarm`. **Change it** (`passwd`), because SSH is on.

## Risks

- **Never relock the bootloader** while a custom kernel is on the phone or AVB is off.
  A relocked bootloader with modified partitions bricks the phone.
- Flashing `userdata` deletes all Android data. Nothing OS on slot a cannot read the ext4
  `userdata` either. To go back, reflash stock with `scripts/flash-stock.sh`.
- Back up the modem partitions (`nvdata`, `nvcfg`, `protect1`/`protect2`, `persist`)
  before experimenting. They hold the IMEI and RF calibration.
- No suspend: expect roughly a day with the screen mostly off.
- No warranty. This is a hobby project.

## Gotchas

- **Crash guard.** If the GPU session dies within 90 s of starting, the next start
  falls back to software Phosh for that boot. If you restart `tetris-phosh` within
  90 s, you trigger it too.
- **GTK4 apps on libhybris EGL.** They can crash when Android threads exit. Chats, Calls
  and Kamera therefore render in software.
- **Don't `depmod` over the whole vendor module set.** Duplicate exports make it pick
  the wrong modules. Out-of-tree modules are loaded with `insmod`.

## Credits

[Nothing](https://github.com/NothingOSS) for the kernel sources;
[libhybris](https://github.com/libhybris/libhybris),
[Halium](https://halium.org) and the [LuneOS / webOS-ports](https://github.com/webOS-ports)
team for the Android 16 libhybris work and GSI; [Droidian](https://droidian.org) for
the original wlroots hwcomposer backend idea; [Phosh](https://phosh.mobi);
[spike0en/nothing_fastboot_flasher](https://github.com/spike0en/nothing_fastboot_flasher)
for the stock partition lists.

## License

Our own code (scripts, services, patches, apps) is **GPL-2.0-or-later** (see
`LICENSE`). Patches to other projects keep those projects' licenses: Linux and
ModemManager (GPL-2.0), wlroots (MIT), libhybris (Apache-2.0).
