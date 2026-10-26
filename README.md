# Vestel Venus 5.5x — Headless Alpine Linux Node

[![SoC: Qualcomm MSM8926](https://img.shields.io/badge/SoC-Qualcomm%20MSM8926%20(Snapdragon%20400)-blue.svg)](#hardware)
[![Kernel: stock 3.4](https://img.shields.io/badge/Kernel-stock%20Android%203.4-green.svg)](#status)
[![OS: Alpine Linux](https://img.shields.io/badge/OS-Alpine%20Linux%20armv7-purple.svg)](#how-it-boots)

Bring-up notes and tooling for turning a screenless **Vestel Venus 5.5x** (`VSP355s`) board into a headless Alpine Linux node: USB RNDIS networking, SSH, Qualcomm WCNSS/Pronto Wi-Fi, persistent storage and a working `apk`. The board is a bare motherboard (no screen, no touch, no battery, no UART access), so all work is done over Qualcomm EDL and USB.

Alpine runs on the **stock Android 4.4.2 kernel (3.4.0)**. The stock `boot.img` ramdisk is replaced by a custom static `/init` that mounts `userdata` and pivots into an Alpine/OpenRC rootfs.

The full engineering narrative, including dead ends and corrected conclusions, is in [docs/BRING-UP-LOG.md](docs/BRING-UP-LOG.md).

---

## Hardware

| | |
|---|---|
| **SoC** | Qualcomm Snapdragon 400 **MSM8926** (4× Cortex-A7, armv7 / 32-bit, Adreno 305); reference-board family `MSM8226` |
| **RAM** | 1 GB (848 MB visible to the stock kernel) |
| **Storage** | eMMC, 30,535,680 × 512 B sectors (~14.6 GiB), 25 partitions |
| **Stock ROM** | `Vestel/s55/s55:4.4.2/KVT49L/VSP355s_V0.64bs`, kernel `3.4.0 armv7l` |
| **Wi-Fi** | Qualcomm WCNSS / Pronto (Prima family) |
| **Debug UART** | `ttyHSL0` @ `0xf9922000` per dmesg, but the pins were not found and the stock console is not routed to it |
| **Power** | bench PSU on the battery terminals; a full boot needs battery-terminal power, EDL works on USB VBUS alone |

## Status

| Feature | State |
|---|---|
| EDL access | ✅ Volume Up + Volume Down while plugging USB → `05c6:9008` |
| Full eMMC backup | ✅ taken before any write (not published) |
| Root shell without UART | ✅ patched `recovery.img` (adbd forced on, no auth) |
| Custom PID 1 from the stock kernel | ✅ verified with readback-checked probes |
| Ramdisk pivot onto `userdata` (ext4) | ✅ |
| Alpine + OpenRC | ✅ boots, persistent across reboots |
| USB RNDIS networking | ✅ comes up automatically at boot |
| SSH | ✅ dropbear (OpenSSH's sandbox needs `PR_SET_NO_NEW_PRIVS`, added in Linux 3.5) |
| Interactive login, `apk` | ✅ `devpts` service and `/etc/apk/repositories` |
| Wi-Fi (WCNSS/Pronto) | ✅ `wlan0` scans, associates and gets DHCP automatically from cold boot |
| Mainline kernel / pmOS | ❌ not attempted; there is no device tree for this board |

---

## How It Boots

```
[PBL/SBL/aboot] → stock boot.img (kernel 3.4.0 + custom ramdisk)
  └── /init (static ARMv7 ELF, scripts/init_final.c)
        ├── creates device nodes (no devtmpfs, no mdev/udev on this kernel)
        ├── mounts userdata (mmcblk0p25) at /newroot
        └── pivots into it as PID 1 → Alpine OpenRC
              ├── vestel-usb-net  (android_usb RNDIS gadget)
              ├── vestel-wifi-hw  (WCNSS firmware helper + module bring-up)
              ├── vestel-devpts   (mounts /dev/pts)
              └── dropbear, wpa_supplicant, udhcpc
```

Notable lessons (details in the log):

- A probe that reported "zero progress" was itself buggy (no `/dev` in the ramdisk, swallowed errors); trust only syscalls whose return values and errno are checked, and verify flashed images by readback.
- Wi-Fi needed three unrelated fixes: config files under `/lib/firmware` rather than `/etc/firmware`, the real `wcnss.mdt` + `b00`…`b09` segments from the `modem` partition image (not the standalone `wcnss.mbn`), and a firmware helper that loops on `write()` because the sysfs firmware `data` write is capped at `PAGE_SIZE`.
- The `userdata` ext4 image must use only features the 3.4 kernel mounts (see the log for the exact `mkfs.ext4` flags).
- Host-side NetworkManager can silently reset the RNDIS interface address; check `ip addr` before assuming the board hung.

---

## What You Need to Supply

None of the following is in this repository:

- the stock firmware package for the `VSP355s` (firehose programmer `prog_emmc_firehose_8x26.mbn`, `rawprogram0.xml`, partition images)
- an EDL client such as [bkerler/edl](https://github.com/bkerler/edl); this board needs `--sectorsize=512`
- a full backup of the stock eMMC, taken **before** the first write: it is the only rollback path
- an armv7 Alpine rootfs (built with `pmbootstrap chroot -b armv7` in this project) and an ARM cross compiler or `zig cc -target arm-linux-musleabihf -static` for the static helpers
- your own SSH keys and Wi-Fi credentials (never baked into an image; write `/etc/vestel-wpa_supplicant.conf` over SSH after flashing)

Warning: this flashes a real device. Back up first, and read the log before writing anything.

## Repository Layout

```
docs/BRING-UP-LOG.md         full chronological bring-up log
docs/DIAGNOSTIC-REVIEW.md    independent review that corrected the early custom-/init conclusions
docs/PHASE2-REVIEW.md        independent review of the Phase 2 (Alpine/OpenRC) investigation
scripts/init_final.c         production /init: mount userdata, pivot, exec the Alpine init
scripts/init_pivot_probe.c   readback-verified pivot probe
scripts/patch_boot.py        unpack / repack / patch / replace-ramdisk for boot and recovery images
scripts/vestel_fw_helper.c   firmware loader helper (no mdev/udev on this board)
scripts/vestel_wcnss_trigger.c  opens /dev/wcnss_wlan to trigger WCNSS firmware loading
scripts/vestel_pmu_poll.c    PMU poll helper
scripts/vestel-*.initd/.start   OpenRC services and local.d scripts (USB net, Wi-Fi hardware, devpts, DHCP)
```

## License

Scripts, helpers and documentation are available under the MIT License (see [LICENSE](LICENSE)). Stock firmware, the firehose programmer and ROM packages belong to their respective owners and are not included.
