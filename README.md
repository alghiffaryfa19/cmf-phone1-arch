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
| Audio (speaker, mic) | ✅ | UCM config. The phone has no 3.5 mm jack; analog headsets via a passive USB-C adapter should be detected (codec "Headset Jack"), untested |
| Vibration | ✅ | feedbackd through a force-feedback bridge (`tetris-vibra`) |
| Torch | ✅ | Phosh quick setting (`tetris-torch`, flash LED via uleds) |
| Cameras | ✅ | Main (50 MP), front, and the rear IR depth sensor; own app **Kamera** with an IR false-colour mode |
| Mobile data (LTE) | ✅ | ModemManager with our plugin for the MediaTek modem, IPv4 + IPv6; works without configuring an APN |
| SIM PIN, SMS receive | ✅ | Chats (Chatty) |
| SMS send | ✅ | Chats (Chatty) |
| Calls | ❌ | Incoming calls don't ring: the modem falls back to 2G for the call but never reports it to Linux. Probably a MediaTek init command is missing. No VoLTE (needs MediaTek's IMS stack). Help welcome |
| Standby (suspend) | ❌ | The SoC hangs in deep sleep. The screen can still turn off, but the CPU stays awake |
| GPS | 🟡 | MediaTek GNSS HAL + `tetris-gnss` feed geoclue (receiver on only while an app asks). Satellites are found, a first fix not confirmed yet |
| Fingerprint | 🟡 | Under-display sensor works with `tetris-fp enroll` / `verify` (Goodix HAL + Trustonic TEE); unlocking Phosh with it not yet (touch issue). The phone has no NFC |

## How it is put together

- **Kernel:** Nothing's open source kernel and MediaTek vendor modules
  ([NothingOSS](https://github.com/NothingOSS)), built with Android's clang and plain
  `make`. There are a few small patches in `patches/kernel*`.
- **Boot:** The kernel goes to `boot_b`. A small busybox initramfs goes into
  `vendor_boot_b`; it loads the vendor modules in stock order and mounts the Arch rootfs
  from `userdata`. Slot a keeps Nothing OS. The bootloader has no `fastboot boot`, so
  every test means flashing slot b.
- **Halium container (LXC):** a LuneOS Halium 16 GSI runs the vendor HALs: gralloc,
  composer, modem (`ccci_mdinit`), camera, power, codec2 and GNSS. It runs in its own network
  namespace and cannot suspend the host.
- **GPU:** a wlroots 0.20 **hwcomposer backend**, a gralloc allocator and
  `android_wlegl` client buffers (`patches/wlroots/`). phoc composites on the Mali GPU
  and hands frames to Android's composer HAL.
- **Modem:** a tty bridge kernel module (`tetris_mdtty`) and a ModemManager plugin for
  MediaTek's AT and data interface (`patches/modemmanager/tetris`).
- **Own services** (`rootfs/overlay`, `rootfs/src`): Phosh session with GPU/software
  selection and crash guard, `tetris-sensors`, `tetris-vibra`, `tetris-torch`, the
  camera stack (`tetris-camera`), the **Kamera** app (Camera2 NDK via libhybris) and
  `tetris-gnss` (GPS NMEA for geoclue).

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

After the first boot the **screen stays black**. That is expected: Arch's stock
wlroots lacks phoc's layer-shell patch, and Phosh exits. The second stage fixes this.
The phone is reachable over USB at `172.16.42.1`. Give the PC side a static
`172.16.42.2/24`, e.g. `nmcli c add type ethernet con-name tetris-usb match.driver cdc_ncm
ipv4.method manual ipv4.addresses 172.16.42.2/24 ipv6.method ignore`. Give it internet (`sudo
scripts/host-nat.sh up` shares the PC's connection over USB) and run the second stage.
It builds libhybris, wlroots and the ModemManager plugin on the phone and installs the
Halium image:

```sh
scripts/setup-phone.sh /path/to/halium/system.img
```

The user is `alarm` with password `alarm`. **Change it** (`passwd`), because SSH is on.

## Server profile (branch `server`)

The phone as a small always-on server: no Phosh, panel off, reachable over USB and WLAN,
runs Docker containers and local AI models (Ollama). `tetris-mode` switches between
the server and the phone profile; both stay installed.

| | |
|---|---|
| Kernel | Docker options on top of the phone config (`kernel/linux.config`, "Server profile"): nftables + iptables compat, `br_netfilter`, `xt_addrtype`, IPv6 NAT, CFS bandwidth (`--cpus`), IPVS, VXLAN/IPVLAN |
| Display | `tetris-display-off` switches the panel off (DRM DPMS) and holds DRM master |
| Network | USB: `172.16.42.1` (as before). WLAN: NetworkManager, power save **off** (it delays incoming connections by a beacon interval). mDNS name `tetris.local` (Avahi) |
| SSH | Keys only over WLAN, password too over the USB cable (`etc/ssh/sshd_config.d/50-tetris-server.conf`). The project key and the PC user's keys are installed |
| Docker | `overlay2` on the ext4 rootfs, logs capped at 3 × 10 MB per container, `live-restore`. `alarm` is in the `docker` group |
| AI | Ollama as a container (`/opt/tetris-server/ai/compose.yaml`, `tetris-ai.service`), API on port 11434, CPU only. Optional web chat (Open WebUI, port 3000) |
| Memory | zram swap (half the RAM, zstd), `systemd-oomd`. The Halium container stays off (frees its RAM) unless you keep the modem |
| Off | Phosh, sensors, vibration, torch, GPS, fingerprint, Bluetooth, modem (keep it with `--with-modem`) |

### Setting it up

The server kernel must be on slot b first (all modules are rebuilt, `vendor_boot_b` too):

```sh
git switch server
scripts/build-kernel.sh && scripts/build-debug-boot.sh
scripts/flash-debug-b.sh            # phone in fastboot
```

Then either convert the installed phone (keeps `userdata`; needs internet on the phone).
The new kernel refuses the old modules in the rootfs, so push the new ones first:

```sh
scripts/push-modules.sh             # modules matching the flashed kernel
scripts/setup-server.sh             # or --with-modem for mobile data as uplink
```

or build a fresh server image: `sudo PROFILE=server scripts/build-rootfs.sh` and flash
it to `userdata` as described above (the second stage, `setup-phone.sh`, is only needed
for the phone profile and for the modem).

### Using it

```sh
ssh alarm@tetris.local                               # or alarm@172.16.42.1 over USB
docker context create tetris --docker host=ssh://alarm@tetris.local
docker --context tetris run --rm hello-world         # Docker from the PC, runs on the phone
docker --context tetris exec -it ollama ollama pull qwen3:1.7b
curl http://tetris.local:11434/api/generate -d '{"model":"qwen3:1.7b","prompt":"Hallo"}'
```

- `tetris.local` needs mDNS on the PC (`nss-mdns`, `mdns_minimal` in `/etc/nsswitch.conf`);
  without it use the WLAN address.
- The first boot pulls the Ollama image (~3 GB, takes a while); `journalctl -u tetris-ai`.
- Images must be `linux/arm64`. For x86-only images, build arm64 ones yourself
  (`docker buildx build --platform linux/arm64`).
- AI models run on the CPU (4 × Cortex-A78 + 4 × Cortex-A55). Models of 1–4 B
  parameters in Q4 are practical; RAM decides the upper limit. Pull models with
  `tetris-ollama-pull <model>`: it pins them to 4 threads, ~20 % faster than Ollama's
  default of 8 (the A55 cores hold the A78 ones back). qwen3:1.7b: ~12 tokens/s.
- **GPU (experimental):** the vendor Mali driver works through libhybris for OpenCL 3.0
  and Vulkan 1.3. llama.cpp's OpenCL backend only accepts Adreno/Intel, its Vulkan
  backend is generic: `pkgbuilds/build-llama-vulkan.sh` (on the phone) builds it,
  `tetris-llama llama-bench -m model.gguf -ngl 99` runs it (container must be up for
  `/vendor`). The GPU is small (2 cores, 1 GHz), so don't expect miracles; token
  generation is limited by memory bandwidth on both. The MediaTek APU (NPU) only works
  with NeuroPilot-compiled models under Android, and its kernel drivers hang here.
- The phone runs on the charger all the time: keep it cool and out of direct sun.
- Back to the phone: `sudo tetris-mode phone && sudo reboot`.

## Desktop profile (branch `desktop`)

The server profile plus a full GNOME desktop that you use over **RDP** from a PC, tablet
or another phone (Remmina, FreeRDP, Windows' Remote Desktop, Microsoft's RDP apps). The
panel stays off; GNOME runs as a headless session of `alarm`, **renders on the Mali GPU**
(libhybris), and RDP streams it with H.264/RemoteFX.

| | |
|---|---|
| Kernel | Everything from the server profile, plus a backport of `SO_PEERPIDFD` from Linux 6.5 (`patches/kernel/0001`). GNOME 49+ needs peer pidfds from dbus-broker: without them the GDM polkit rule for headless sessions and the remote-login handover fail ("D-Bus broker doesn't support pidfd") |
| Session | `gnome-headless-session@alarm` (GDM starts it at boot, no display needed); `72-tetris-headless-seat.rules` takes the panel out of seat0 so GDM opens no login screen on it |
| RDP | `gnome-remote-desktop` in headless mode in alarm's session, port 3389. Certificate and a random password are made on the first boot (`tetris-rdp-setup`, user unit); the password is in `~/.config/tetris-rdp-password` |
| GPU | libhybris is built a second time as a **glvnd EGL vendor** (`/opt/libhybris-glvnd`) and listed in front of Mesa for the session (`user-environment-generators/60-tetris-hybris-gl`). glvnd picks per EGL platform: mutter's surfaceless display and the apps' Wayland EGL go to the Mali driver (mutter then offers `android_wlegl` to clients), Xwayland's GBM goes to Mesa. Needs `patches/libhybris/0002` (surfaceless platform) and the TLS fix in `0001`, the Halium container (gralloc) and a larger glibc static TLS reserve (`GLIBC_TUNABLES`). Verified: GNOME Shell, GTK 4 apps and X11 GL apps (glxgears) on the Mali-G615 |
| Fallback | Without the container, glvnd uses Mesa. A GPU shell that dies within 60 s switches the session to software for that boot (`tetris-gnome-shell`). Off for good: `touch ~/.config/tetris-no-gpu` |
| Memory | ~850 MB used after boot with GNOME, Docker and Ollama idle (CPU rendering; the container adds its share with the GPU) |

### Setting it up

```sh
git switch desktop
scripts/build-kernel.sh && scripts/build-debug-boot.sh
scripts/flash-debug-b.sh            # phone in fastboot
scripts/push-modules.sh             # modules for the new kernel (when coming from main)
scripts/setup-desktop.sh            # server + desktop profile, prints the RDP password
```

For the GPU the Halium image must be installed (`scripts/setup-phone.sh`, as for GPU
Phosh); `setup-desktop.sh` then rebuilds libhybris with its glvnd variant. A fresh image
with the desktop profile: `sudo PROFILE=desktop scripts/build-rootfs.sh`, then
`setup-phone.sh` for the GPU.

Then connect to `tetris.local:3389` (or `172.16.42.1` over USB, or the WLAN address)
with user `alarm` and the RDP password, e.g. `xfreerdp3 /v:tetris.local /u:alarm
/dynamic-resolution`. The session is the logged-in GNOME of `alarm`; the desktop keeps
running when you disconnect. New RDP password: `tetris-rdp-setup` on the phone.
Back: `sudo tetris-mode server` or `sudo tetris-mode phone`, then reboot.

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
