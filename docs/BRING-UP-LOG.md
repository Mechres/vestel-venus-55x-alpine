# Vestel Venus 5.5x

> **Phase 2 complete (2026-09-08): headless Alpine + SSH working end-to-end
> on real hardware.** Ramdisk-pivot boots a real Alpine/OpenRC rootfs, USB
> RNDIS networking comes up automatically, and key-based SSH login works.
> Root cause of the long SSH debugging chase: this kernel is Linux 3.4.0,
> one version before `PR_SET_NO_NEW_PRIVS` was added (Linux 3.5) — OpenSSH's
> privilege-separation sandbox calls that `prctl()` unconditionally and
> treats failure as fatal, silently killing every connection's auth child
> before it could even send its banner. Fixed by switching to **dropbear**
> (no such sandbox requirement), confirmed via direct debug-mode testing on
> the real kernel before touching the production image. Two other real bugs
> were found and fixed along the way (device-node permissions masked to
> 0644 by an unset `umask`; slow kernel entropy accumulation on a bare
> board with no rngd) — neither was *the* cause of the SSH crash, but both
> are now fixed regardless. Persistence and repeatable cold boots verified
> directly: `/var/log/vestel-boot.log` shows one line per boot across two
> consecutive boots, and a test file written over SSH survived a reboot
> intact. See "Phase 2" below for the full diagnostic trail — it's a good
> reference for the next round of Alpine bring-up work (WiFi, Phase 3) if
> anything in that chain needs revisiting. The full [DIAGNOSTIC-REVIEW.md](DIAGNOSTIC-REVIEW.md)
> and [PHASE2-REVIEW.md](PHASE2-REVIEW.md) documents remain useful reading
> for the debugging methodology (bounded retries, checked syscalls, local
> chroot reproduction before hardware, direct foreground-daemon debug
> output) — several real bugs were only found because those reviews caught
> methodology gaps in earlier, over-confident conclusions.

> **Phase 3 complete (2026-09-08): WiFi working end-to-end on real
> hardware, fully automatic from cold boot.** `wlan0` comes up and returns
> real AP scan results with zero manual intervention, confirmed across
> three separate runs including two independent cold boots. Three real,
> unrelated bugs, not one: (1) WCNSS driver config files placed at
> `/etc/firmware/...` (mirroring their path *inside* `system.img`) instead
> of `/lib/firmware/...` (the kernel's actual lookup path); (2) the wrong
> PIL firmware entirely — a standalone `wcnss.mbn` found in the firmware
> package's top level turned out to be an unrelated file, while the real
> `wcnss.mdt` + 8 segment blobs (`wcnss.b00`...`b09`) live inside
> `NON-HLOS.bin`, the actual `modem` GPT partition image; (3) this
> device's custom firmware-loading helper (needed because there's no
> `mdev`/`udev` at all) had its own bug — the kernel's firmware `data`
> sysfs write is capped at `PAGE_SIZE` per syscall, so blobs over 4KB were
> silently truncated until the helper looped on `write()`'s actual return
> value. Along the way, one of the apparent "board crashes" that
> triggered a recovery power-cycle turned out to very likely be a
> host-side NetworkManager artifact (silently resetting the RNDIS
> interface's IP), not a real device hang — see "Phase 3" below for the
> full trail, including how that was told apart from the genuine
> findings.

- **SoC (CONFIRMED 2026-08-22):** Qualcomm Snapdragon 400 **MSM8926**
  (4× Cortex-A7, **armv7 / 32-bit**, Adreno 305 GPU)
- **RAM:** 1 GB
- **Count:** 1 (bare motherboard, no screen/touch)

## ⚠️ Tool-path correction
The original research notes (§6) guessed MediaTek **or** Spreadtrum/Unisoc →
SP Flash Tool / Research Download. **Wrong.** This is **Qualcomm**, so:
- ❌ SP Flash Tool / Research Download do **not** apply.
- ✅ Screenless flash path = **Qualcomm EDL (9008)**. Requires the device's
  signed **firehose / rawprogram** loader. Extraction/availability of that
  loader is the real gate (EDL itself needs no screen).
- If the bootloader is unlockable, **fastboot** is also an option.

## pmOS / mainline status
- The **Snapdragon 400 family** has a generic pmOS port: **`qcom-msm8226`**
  (testing). MSM8926 shares this SoC family (modem differs from 8226).
- **But the specific Venus 5.5x board is NOT ported** → needs a **device DTS**
  + bring-up (UART console first, then clock/pinctrl/MMC/USB). Medium-high effort.
- **armv7 (32-bit)** — limits software; fine for a minimal headless node.

## Constraints
- 1 GB RAM → light tasks only; no RandomX.
- 32-bit → aarch64 binaries won't run.

## Action checklist
- [x] Obtain EDL access (test point / key combo) + the device's firehose loader.
  Volume Up + Volume Down while plugging USB → `05c6:9008`.
- [x] Verify the local firmware dump's partition layout against the real device
  (2026-09-08: `printgpt` matches `rawprogram0.xml` exactly on every
  partition offset/length — see "Confirmed facts" below).
- [x] Full stock eMMC backup before any write (2026-09-08, see below).
- [x] Get a root shell without a physical UART (2026-09-08, see "Root shell
  via patched recovery" below) — this replaces "get UART early console" as
  the near-term unblocker; UART is now a nice-to-have for a live boot
  console, not a hard requirement to make progress.
- [ ] Identify exact Venus 5.5x board/variant name (not just marketing name).
- [ ] Search pmOS DTS tree + `#msm8226-mainline` for any existing Venus work.
- [x] Decision (2026-09-08): pursue a **ramdisk-pivot Alpine port on the
  stock/vendor kernel** (1 GB RAM makes stock Android 4.4.2 painful) rather
  than a full mainline pmOS port. See "Alpine ramdisk-pivot bring-up
  (in progress)" below — currently blocked on a custom `/init` not running
  at all on real hardware despite working under emulation.
- [ ] If pursuing pmOS instead/later: write a minimal `msm8926-vestel-venus*.dts`;
  physical UART is `ttyHSL0` @ MMIO `0xf9922000` (GSBI/HSL block, confirmed
  via `dmesg` on-device — see below) but stock kernel cmdline doesn't route
  console there, so pin-level discovery + a cmdline patch would both be
  needed for a live boot log.

## Confirmed facts (2026-09-08)

- **GPT matches the dump exactly.** `edl.py printgpt` against the live board
  matches `rawprogram0.xml`'s partition table on every offset and length
  (modem, sbl1/sbl1bak, sdi, aboot/abootbak, rpm/rpmbak, boot, tz/tzbak,
  modemst1/2, system, persist, cache, recovery, misc, fsg, fsc, ssd, splash,
  userdata). Total disk: 30,535,680 sectors × 512 B = 15,634,268,160 B
  (~14.6 GiB). Sahara handshake reports `HWID 0x008050e1..., CPU "MSM8926"`.
  **Sector size is 512, not the tool's 4096 default** — always pass
  `--sectorsize=512` to `edl.py` for this board (`rf`/`w` silently no-op or
  misbehave otherwise).
- **Full stock backup**: `backups/vestel_venus55x_full_emmc_20260908.bin`
  (15,634,268,160 bytes, gitignored). SHA256:
  `5d755dc3abee60eb5140795fcea9292c90a55bcda0058cf381d713effefb9082`. This is
  the only rollback path (single unit, no spare) — restore with
  `edl.py --loader prog_emmc_firehose_8x26.mbn --sectorsize=512 wf <backup>`
  before assuming a from-scratch re-flash of the factory dump is equivalent.
- **Board identity**: stock ROM is `Vestel/s55/s55:4.4.2/KVT49L/VSP355s_V0.64bs`
  (Android 4.4.2 KitKat, kernel 3.4.0, `ro.product.board=MSM8226`,
  `ro.board.platform=msm8226`). The DTB only carries Qualcomm's *stock*
  MSM8226 reference-design compatibles (`qrd`, `mtp`, `cdp`, `fluid`, `sim`)
  — no Vestel/ODM-specific dtsi string leaked in `aboot`/`boot.img`, so this
  is a lightly-customized QRD/MTP reference board, not a from-scratch layout.
  (The `venus` strings in `boot.img` are Qualcomm's video codec IP block
  name — coincidental match with the marketing name, not a useful lead.)
- **Secure boot fuses are present but not enforced against unsigned
  `boot`/`recovery` images**: Sahara reports a non-empty `PK_HASH`, but an
  unsigned, hand-patched `boot.img` booted successfully (see below) — so
  don't assume EDL writes to `boot`/`recovery` will be rejected on this unit.

## Root shell via patched recovery (no UART needed)

Finding physical UART test points blind, with no schematic, turned out to be
the hard blocker — not the software side. The path that actually worked
skips UART entirely:

1. **First attempt (`boot.img` patch) hit a dead end.** Patched stock
   `boot.img`'s ramdisk (`default.prop`: `ro.adb.secure=0`,
   `ro.debuggable=1`, `persist.sys.usb.config=mtp,adb`) and flashed it — it
   booted fine (confirms secure boot doesn't block this), but the device
   only ever exposed plain USB Mass Storage (`1e0e:cea5`), never adb. Root
   cause: this is a *used* stock device — `persist.*` properties already
   exist in `/data/property` from real usage, and Android's
   `load_persistent_properties()` overrides `default.prop`'s `persist.*`
   values at boot; then the full framework's own USB/debug management
   re-asserts its cached config later regardless of any `init.rc`-level
   override. Appending a forced `on boot: write .../functions adb; start
   adbd` to `init.rc` still lost to the framework once it finished booting.
   **Lesson: don't fight `persist.*` properties or the framework on a
   previously-used device — go around them.**
2. **`recovery.img` has no framework at all** and its stock `init.rc`
   *unconditionally* sets the USB gadget to `18D1:D001`, `functions=adb` at
   `on init` (before `/data` is even mounted — no persisted-property
   interference possible). It only needs `ro.debuggable=1` (to fire
   `on property:ro.debuggable=1` → `write enable 1; start adbd`) and
   `ro.adb.secure=0` (skip the RSA host-key prompt). Patched only
   `default.prop` in `recovery.img`'s ramdisk — no `init.rc` changes needed.
3. **Booted into recovery deterministically via the BCB**, not a guessed
   button combo: wrote `boot-recovery\0` + zero padding as the first 32
   bytes of the `misc` partition (the same mechanism `adb reboot recovery`
   uses), then a plain `reset`. This is robust regardless of unknown/
   unreliable recovery key combos on this specific model.
   **The BCB is still set to `boot-recovery` as of this writing** — the
   device will keep booting into recovery on every power-on until it's
   cleared (`dd if=/dev/zero of=/dev/block/mmcblk0p20 bs=512 count=4` from
   the recovery root shell, or rewrite `misc` via EDL) or recovery itself
   consumes/clears it.
4. **Recovery's `adbd` execs a hardcoded `/system/bin/sh`**, which doesn't
   exist (recovery's `/system` is just an empty `mkdir`'d tmpfs dir, no real
   partition mounted) — every `adb shell` call fails with
   `exec '/system/bin/sh' failed` until that path exists. Fixed by `adb
   push`-ing a static ARMv7 busybox (borrowed from
   a static ARMv7 busybox from another project, statically linked so no libc
   ABI concerns) to `/system/bin/sh`, then to `/system/bin/busybox`, then
   running `/system/bin/busybox --install -s /system/bin` for a full applet
   set. **`adb push` cannot create missing parent directories** — pushing
   straight to `/system/bin/sh` fails until `/system/bin` exists by some
   other means (in our case, a half-failed recursive-directory push attempt
   left the directory behind even though the file copy itself errored).
   Also: `PATH` is not set to include `/system/bin` by default in this
   shell — `export PATH=/system/bin:$PATH` first, or use full paths.
   **This shell is ephemeral** — it's pushed into tmpfs and vanishes on
   every reboot; re-run the busybox push after each boot into recovery.
5. **Result**: full root shell (`uid=0 gid=0`) confirmed via `adb shell`,
   no auth prompt, no UART. Device state confirmed healthy from inside:
   848 MB RAM, kernel `3.4.0 armv7l`, 25 `mmcblk0pN` partitions matching the
   GPT/`rawprogram0.xml` exactly, plus `mmcblk0rpmb`.
   Debug UART is `ttyHSL0` @ MMIO `0xf9922000` per `dmesg`
   (`msm_serial_hsl_probe: detected port #0`), but stock console is on the
   dummy framebuffer console (`tty0`), not routed to it — a live boot
   console would still need a cmdline patch plus finding the physical pins.

Reusable tooling: `scripts/patch_boot.py` — a corrected
fork of an earlier boot-image patching script (that script's cpio "newc"
parser mis-computes post-filename padding, ignoring the 110-byte header, and
silently only recovers one file from any real multi-entry ramdisk — fixed
here, but not back-ported to the original, which belongs to a different device). Supports `unpack <img> <dir>`,
`repack <dir> <img>`, `patch <img> <out_img>` (patches `default.prop` +
appends the forced-adb `init.rc` block — appropriate for `boot.img`-style
ramdisks; for `recovery.img` just call `patch_default_prop()` directly and
skip the `init.rc` append, since stock recovery already forces the gadget
unconditionally).

## Alpine ramdisk-pivot bring-up (in progress, 2026-09-08)

**Goal**: replace stock Android with a minimal Alpine userspace on top of the
*stock/vendor 3.4.0 kernel* (proven to boot, has working eMMC/USB drivers) —
not a full mainline pmOS port. GM8's own approach (wholesale-replacing
`system_a`'s contents, relying on the bootloader's `skip_initramfs`/
system-as-root behavior) **does not apply here**: this is a 2015 Android
4.4.2 KitKat device, predating SAR. `boot.img`'s ramdisk is the real root at
boot time, so the equivalent move is a custom `/init` that mounts a real
partition and `switch_root`s into it — the plan is described in the sections below.

**Confirmed groundwork facts**:
- Target partition for the new rootfs: `userdata` (`mmcblk0p25`, ~13 GB,
  ~10.9 GB free before repurposing — wipes the phone's remaining stock app
  data, expected/fine for a headless-node conversion).
- `userdata`'s **current** ext4 feature set (the exact flags the stock
  3.4.0 kernel is proven to mount, so any replacement image should match):
  `compat: has_journal,ext_attr,resize_inode` /
  `incompat: filetype,extents` / `ro_compat: sparse_super,large_file,
  uninit_bg`. Notably **absent**: `dir_index`, `flex_bg`, `64bit`,
  `extra_isize`, `metadata_csum` — modern `mke2fs` (host has 1.47.4) enables
  several of these by default and they must be explicitly disabled via
  `-O ^feature,...`, e.g.:
  `mkfs.ext4 -b 4096 -O ^dir_index,^huge_file,^flex_bg,^metadata_csum,^metadata_csum_seed,^64bit,^dir_nlink,^extra_isize,^orphan_file,has_journal,ext_attr,resize_inode,filetype,extents,sparse_super,large_file,uninit_bg -d <staged-dir> <img>`.
- WiFi chipset: **Qualcomm WCNSS/Pronto/Prima** family — same as GM8
  (`wlan.ko` + `pronto_wlan.ko` kernel modules, `wcnss_service`/
  `wcnss_filter` userspace daemons, firmware under stock
  `system.img`'s `etc/firmware/wlan/prima/`). GM8's `HANDOFF.md` documents a
  working bring-up sequence for this exact chip family — deferred until the
  ramdisk-pivot mechanism itself works.
- **EDL only stays connected for ~5 seconds once the bench PSU is supplying
  battery-terminal power** (confirmed on the bench 2026-09-08) — with only
  USB VBUS present EDL is stable, but once real battery power is also live
  the PMIC appears to force a transition onward after a few seconds of EDL
  inactivity. Flash-then-reset command pairs must be issued back-to-back
  within one EDL session; expect to re-enter EDL (Volume Up + Volume Down)
  between most individual operations during this kind of bring-up work.
- A **full Android boot needs battery-terminal power, not just USB VBUS** —
  confirmed early in the recovery work: the board sat completely
  silent on USB for 60+ seconds until bench PSU was connected to the battery
  terminals, at which point it booted immediately. EDL/Sahara/firehose
  itself works fine on USB power alone.

**Blocker history and resolution (see [DIAGNOSTIC-REVIEW.md](DIAGNOSTIC-REVIEW.md)
for the full independent review that corrected this investigation):**
early attempts at a from-scratch custom `/init` in `boot.img`'s ramdisk
appeared to leave **zero trace** on real hardware — a raw breadcrumb-write
scheme (writing a marker to a fixed `misc`-partition offset, independent of
`userdata` mounting) showed no progress across four variants: a shebang
shell script `/init`; the same with the *stock* 51-entry ramdisk otherwise
kept intact; a real static ELF (cross-compiled with `zig cc -target
arm-linux-musleabihf -static`, no ARM gcc cross-toolchain being installed)
that `execve()`s busybox directly, on the theory this kernel lacks
`CONFIG_BINFMT_SCRIPT`; and a single-syscall-only ELF with no busybox/shell
at all. All four were validated under `qemu-arm-static` + `unshare --mount
--pid --fork` + a loopback ext4 image first, including a full `mount` →
write → `switch_root` → post-pivot write round-trip — yet showed nothing on
hardware.

**That "zero progress" conclusion was wrong**, per the independent review:
the single-syscall test's ramdisk never actually contained a `/dev`
directory, so its `mknod`/`open` calls failed with ENOENT and it silently
swallowed the error before idling — a probe bug, not evidence the kernel
never ran it. The review also found the local emulation "confirmation" of
that same test validated a different device path than the one the binary
actually opened (so a live idle process there never proved the write
worked), confirmed via extracted IKCONFIG that this kernel's
`CONFIG_DEVTMPFS` really is unset (a real bug in every `mount -t devtmpfs`
call across all four variants, though not by itself sufficient to explain
every silent marker), and debunked the `CONFIG_BINFMT_SCRIPT` theory
(Linux 3.4 builds shebang support unconditionally — confirmed independently
afterward by directly exec'ing a shebang script from the working recovery
shell on this exact hardware, which worked fine).

**Following the review's checklist — check every syscall's return value
and errno (only trust errno when the call actually failed, a bug caught
during validation), retry `open()` for a bounded period since node
existence doesn't imply driver readiness, verify the flashed image by
readback, and use a marker confirmed absent beforehand — a corrected probe
was flashed and DID leave its marker after a `reset`.** `mkdir("/dev")`
correctly failed with the ramdisk's own `/dev` already present; `mknod`
succeeded; `open()` needed one retry (first attempt returned `ENXIO`,
succeeded ~50ms later — a real, previously-untested timing detail, not
assumed); `lseek`/`write`/`fsync` all succeeded; and the exact fresh marker
string (verified absent immediately beforehand) was intact after the
reset. **The ramdisk-pivot mechanism itself is confirmed working**: this
kernel's stock/vendor `boot.img` boots a from-scratch custom `/init` as
PID 1 correctly, including a raw block-device write that survives an
abrupt EDL reset once explicitly `fsync()`'d.

**Full pivot re-verified on hardware with the same rigor (2026-09-08),
milestone reached.** Per further review feedback, "custom PID 1 runs and
writes persist" and "the full mount+pivot actually works" are distinct
claims — the first was proven above, the second needed its own dedicated,
equally-rigorous test: explicit device nodes for both `misc` and
`userdata`, bounded retries on **both** `open()` and the `mount()` call
itself (not just node existence), distinct pre-pivot/post-pivot markers
each independently checked and `fsync()`'d, and — critically — a
post-pivot check that doesn't just assume the pivot worked: `getpid()`
read back directly, and `/proc/mounts` read from inside the new root and
recorded verbatim as evidence rather than a boolean verdict. Flashed image
verified byte-identical by readback before every reset, exactly as before.

Result: every step succeeded. `mknod`+`mount` of `userdata` (ext4) at
`/newroot` succeeded on the first try; the pre-pivot marker was written and
`fsync()`'d; the pivot itself (`chdir("/newroot")` →
`mount(".", "/", NULL, MS_MOVE, NULL)` → `chroot(".")` → `chdir("/")` — the
mechanism `switch_root` wraps) succeeded; **`getpid()` read `1` after the
pivot**, confirmed directly rather than assumed; `/proc` mounted cleanly
post-pivot and `/proc/mounts` read back verbatim as:
`rootfs / rootfs rw 0 0` / `/dev/userdata_blk / ext4 rw,relatime,data...`
— i.e. the userdata device is genuinely serving as `/`; the post-pivot
marker (a distinct string from the pre-pivot one) was written to the new
root's own filesystem and `fsync()`'d; final status marker read back as
`PIVOT-PROBE-COMPLETED-ALL-STEPS-OK`. All of this survived the same
abrupt EDL reset + cold readback cycle as every other check in this
investigation.

**The ramdisk-pivot mechanism for this device is now fully established**:
a from-scratch custom `/init`, booting as PID 1 from this kernel's stock
`boot.img`, can mount a real partition and pivot onto it as root, with
persistent, verified writes on both sides of the pivot. Next phase is no
longer mechanism-proving — it's building the actual intended rootfs
(Alpine packages, OpenRC, networking) on top of this now-proven pivot, per
the original phased plan (Phase 2/3 outline still applies, see the
now-superseded-in-spirit-but-still-useful groundwork facts above: userdata
ext4 feature flags, WCNSS/Pronto WiFi chip identity, the EDL/PSU timing
behavior, and the battery-terminal-power requirement for a full boot).

Reusable tooling: `scripts/patch_boot.py` gained a
`replace-ramdisk <boot.img> <new_root_dir> <out_boot.img>` mode during this work
— wholesale-replaces the ramdisk with a fresh tree (mode/type read from
disk, not a round-tripped manifest) while keeping the stock kernel/DTB.
Verified via `cpio -tv` and full unpack/kernel-`cmp` round-trips each time
before every flash in this investigation. Production `/init` source is
preserved at `scripts/init_final.c`; a known-good pivot
boot.img + its build source + its readback evidence are preserved at
`backups/known-good-pivot/` (local only, not published).

## Phase 2: Alpine/OpenRC bring-up on top of the proven pivot (in progress, 2026-09-08)

> **Independent Phase 2 review:** Read [PHASE2-REVIEW.md](PHASE2-REVIEW.md)
> before the next test. The saved pre-exec marker does not prove successful
> exec: the failure logger used a closed fd. USB currently runs after SSH
> and all other default services, so a service stall can prevent enumeration.
> The review includes a source fix and temporary inittab/runlevel logging
> files; these have not been flashed. The narrative below records the
> preceding investigation and contains assumptions corrected by that review.

**Compatibility confirmed first, as planned**: fetched real, current Alpine
armv7 packages directly from `dl-cdn.alpinelinux.org` (not locally-built —
this tests actual upstream binaries) and ran them on the real stock 3.4.0
kernel via the working recovery+adb shell, *before* committing to a full
build:
- `busybox-static-1.37.0-r31.apk`'s `bin/busybox.static` (musl, static-PIE)
  ran correctly (`echo`, `uname -a`, `id` all worked) — confirms this
  kernel executes current musl/PIE binaries fine.
- `openrc-0.63.2-r0.apk`'s `sbin/openrc` (dynamically linked) ran correctly
  when invoked via `musl-1.2.6-r2.apk`'s `ld-musl-armhf.so.1
  --library-path`, printing `openrc_bin (OpenRC) 0.63.2` — confirms
  dynamic linking against a real musl runtime also works, not just static
  binaries.

**Rootfs built via `pmbootstrap chroot -b armv7`** (already configured on
this machine from GM8's setup): `alpine-base openrc eudev busybox openssh
kmod util-linux` installed with `apk --no-scripts --root ... --initdb`
(post-install scripts fail under this environment, same as GM8's
`build_gm8_root_tar.sh` already documented — fixed up by hand afterward):
`busybox --install -s <dir>` re-run for `/bin`, `/sbin`, `/usr/bin`,
`/usr/sbin` (doing only `/bin` initially broke several absolute-path
references — `/sbin/syslogd`, `/usr/bin/readlink` — used by stock service
scripts); `rc-update add` by hand for a minimal runlevel set (`sysfs
dmesg` in sysinit; `modules sysctl hostname bootmisc syslog localmount` in
boot; `local sshd` in default — **`devfs` and `mdev` deliberately NOT
added**, see below); dedicated ed25519 keypair generated
(`vestel-venus-55x/vestel_node_key` / `.pub`, private key gitignored,
`authorized_keys` baked in for `root`); `sshd_config` set to
`PasswordAuthentication no` / `PubkeyAuthentication yes`; USB RNDIS
networking (`android_usb` sysfs gadget, same mechanism GM8 uses) wired up
as `/etc/local.d/vestel-usb-net.start`, script preserved at
`vestel-venus-55x/scripts/vestel-usb-net.start`.

**`devfs`/`mdev` removed from the runlevel deliberately**: this kernel has
no `devtmpfs` (confirmed absent from `/proc/filesystems` on real hardware,
not just static `.config` extraction), so `devfs`'s `mount -t devtmpfs`
step would fail and (per the busybox `devfs` init script's normal
fallback behavior) very likely falls back to mounting a **fresh tmpfs
over `/dev`** — which would silently shadow whatever real, persistent
device nodes our proven C `/init` had already written to `/newroot/dev`
before the pivot. Rather than depend on `mdev`'s coldplug populating
`/dev/urandom` etc. correctly afterward (unverified, and `mdev`'s own
`persistent-storage` helper has a real bug on this build — see below),
the design now relies entirely on the already-proven, explicit-`mknod`
pattern for the handful of devices real programs need
(`null zero random urandom ptmx`), done once by `/init`, persisting on
the real ext4 filesystem exactly like traditional static `/dev` entries
always did.

**Bugs found and fixed this phase, each confirmed via evidence before
moving on — do not re-introduce these**:
1. USB interface misdetection: the local.d script's naive
   `ls /sys/class/net | grep -v '^lo$' | head -1` picked `dummy0` (a
   virtual interface from this device's modem/RIL stack) instead of the
   real `rndis0`, which *was* present in the list but not first
   alphabetically. Confirmed via a persistent debug log
   (`/var/log/vestel-usb-net.log`, written by the script itself) showing
   the exact interface list at the moment of failure. Fixed by matching
   `^rndis|^usb` explicitly rather than "whatever isn't lo".
2. `sshd` accepted TCP connections (port 22 open, confirmed via `nc -zv`)
   but reset the connection during `kex_exchange_identification` — before
   sending even its protocol banner. Classic symptom of the forked child
   dying immediately on missing `/dev/urandom` (no devtmpfs, no
   verified-working mdev coldplug on this kernel). Not yet re-confirmed
   fixed on hardware as of this writing (see below).
3. `/lib/mdev/persistent-storage: line 57: /usr/bin/readlink: not found`
   (with busybox's help text garbling stdout) on every detected block
   device, and `/sbin/syslogd does not exist` — both because
   `busybox --install` was only run for `/bin` initially. Real Alpine
   service scripts assume specific absolute paths per FHS convention;
   fixed by installing applet symlinks into all four standard directories.
4. **Two parallel rootfs copies fell out of sync during iteration** —
   fixes were applied to `pmbootstrap`'s own chroot path
   (inside the pmbootstrap armv7 chroot) while the
   actual image-build staging directory was a separate `rsync`'d copy
   that had already diverged (kept the fixed `local.d` script, but not
   later `rc-update`/symlink fixes) — a full local `chroot` + `openrc
   sysinit/boot/default` dry run (see below) exposed this immediately
   when the "already-fixed" symlinks turned out not to be present.
   Resolved by treating one side as the single source of truth and
   re-syncing before every build from that point on.

**Local verification methodology** (established this phase, use it before
every future rootfs change): rather than iterating blind on hardware,
copy the staged rootfs, apply the same `mknod` calls `/init` would apply,
then run `unshare --mount --pid --fork` + `chroot <root> /sbin/openrc
sysinit`, then `boot`, then `default` directly, checking exit codes and
full output. This caught bugs 1, 3, and 4 above without touching the
device at all, and confirmed all three runlevels exit 0 cleanly with the
`devfs`/`mdev` removal in place.

**The "complete USB silence" discrepancy was resolved** — it turned out to
be measurement noise, not a regression: the `userdata` v4 build that
showed nothing was actually fine; a subsequent boot of the *same class* of
image (v5, adding an `PHASE2-REVIEW.md`-designed ENTER/WATCH/EXIT boot
trace wrapper around every OpenRC runlevel, logging to
`/var/log/vestel-openrc-trace.log` on `userdata` itself) showed the USB
gadget enumerating correctly and the full boot completing normally. The
real, reproducible problem — confirmed by that same trace — was
**downstream of a successful boot**: `sshd` itself started fine per
OpenRC, but any real client connection got `Connection reset by peer`
during `kex_exchange_identification`, before the server even sent its
banner.

**Root cause, found via direct evidence, not further guessing**: chrooted
into the live `userdata` filesystem from the recovery+adb shell (no
network/EDL round-trip needed for this) and ran `sshd -d -e` in the
foreground. The real debug output was unambiguous:
```
ssh_sandbox_child: prctl(PR_SET_NO_NEW_PRIVS): Invalid argument [preauth]
```
`PR_SET_NO_NEW_PRIVS` was added in **Linux 3.5**; this kernel is
**3.4.0** — one version too old. OpenSSH's Linux privilege-separation
sandbox calls this `prctl()` unconditionally before installing its
seccomp filter and treats failure as fatal, killing the per-connection
auth child immediately. This is a genuine kernel/OpenSSH-sandbox
incompatibility baked into the compiled binary, not something fixable via
`sshd_config`. Two other real bugs were found and fixed along the way
during this investigation, confirmed via direct inspection of the
already-flashed device (neither was *the* cause of the SSH crash, but
both are real and now fixed):
- Device-node permissions (`/dev/null` etc.) were `0644` instead of the
  requested `0666` — `mknod`'s mode was silently masked by the default
  process `umask`, never explicitly set to `0` in `/init`. Fixed with
  `umask(0)` plus explicit `chmod()` after each `mknod` (self-heals nodes
  already created with the wrong mode by an earlier boot, since `mknod`
  is a no-op on an already-existing node).
- `sshd -G`'s dependency-check invocation blocked in `poll_schedule_timeout`
  for the ~12s of `sysinit` while `/proc/sys/kernel/random/entropy_avail`
  climbed slowly (25→60) — OpenSSL's `RAND_poll` heuristic, needed because
  this kernel predates `getrandom()`/`getentropy()` (added in Linux 3.17).
  Fixed in `/init` (`init_final.c`) by seeding the pool via the
  `RNDADDENTROPY` ioctl — confirmed via a standalone test that jumped
  `entropy_avail` from 51 to 1971 immediately on this exact kernel.
  **Provenance, spelled out precisely** (this is not a hardware entropy
  source and must not be read as one): reads live from `/dev/urandom` at
  that exact boot, every boot — never a fixed buffer, never a value
  stored on disk and replayed. On this pre-CRNG-era kernel, `/dev/urandom`
  is fed by whatever real boot-time jitter the kernel has gathered by
  that point — genuinely fresh per boot, but on a bare board this early
  there may be very little of it, so crediting it back doesn't manufacture
  new physical randomness; it only unblocks `entropy_avail`-gated
  consumers, trusting the kernel's own pool output for that purpose. This
  only covers the pre-OpenRC window `/init` runs in. For every boot after
  that, **`seedrng` is now enabled** in the `boot` runlevel (upstream
  kernel.org tool, bundled with OpenRC, already present in the image) —
  the properly-audited version of the same idea: it reads the seed file
  written at the *previous* shutdown/boot, credits it, and immediately
  rotates in a fresh one for next time, so a stored seed is never reused
  twice. Confirmed present and working: `/var/lib/seedrng/seed.credit`
  (mode 0400, root-owned) exists after boot and `entropy_avail` reads
  healthy (141) shortly after. Both mechanisms are appropriate for this
  device's current scope — a bench-wired lab/dev node on a private USB
  link — but neither substitutes for a real hardware RNG source before
  trusting this for anything long-lived or exposed beyond that.

**Fix**: swapped `openssh-server` for **dropbear** (`-s` for key-only
auth), which has no equivalent sandboxing requirement. Confirmed working
via the *exact same* direct chroot+foreground-debug methodology before
ever touching the production image: full key-authenticated SSH connection
over loopback, `exit 0`. `dropbear`'s `depend()` also needed `need net`
changed to `use net` since USB networking is managed by our own
`local.d` script, not OpenRC's own `net.*` service model.

**Confirmed working end-to-end on real hardware (`userdata` v7, `boot`
v7 — v7 adds `seedrng`, no functional change from v6 otherwise)**: USB
RNDIS gadget enumerates automatically, `ping` and key-based SSH both work
(`ssh -i vestel_node_key root@172.16.42.1`), and — the last Phase 2
checklist item — persistence and repeatable boots are directly verified:
a file written over SSH survived a reboot intact, and
`/var/log/vestel-boot.log` shows one line per boot across two consecutive
boots. (One hardware quirk: a software `reboot` from within Alpine left
the board powered-down rather than auto-restarting — likely because this
bench-PSU-wired-to-battery-terminals setup doesn't replicate the PMIC's
normal power-hold sequencing a real battery provides on restart; a manual
power toggle brings it straight back up, and this doesn't affect a normal
EDL-based cold boot/reset cycle at all.)

**Device state as of this writing**: `boot` is still the working v7 build
(preserved at
`vestel-venus-55x/backups/phase2-alpine-attempt/boot_v7_working.img`,
checksummed, along with the final `init_final.c` source) — unchanged by
any Phase 3 work. `userdata` is now **v9** (see Phase 3 section below):
functionally equivalent to v7 for Phase 2 purposes (dropbear + `local`
only actually enabled), plus the staged-but-not-enabled WiFi files.
`misc`'s BCB is clear (boots `boot`, not `recovery`). The recovery+adb
root-shell path (patched `recovery.img`, untouched by any of this) remains
available anytime via the BCB trick as a rollback/debug path independent
of whatever state the Alpine side is in. Full stock backup remains the
complete rollback path to stock Android if ever needed.

**Also noted, not investigated further**: the EDL tool itself threw a raw
Python exception a couple of times this session (`TypeError: a
bytes-like object is required, not 'str'` in `firehose.py`'s
`cmd_read_buffer`, during `connect()`/`configure()`) — a bug in the
vendored `edl.py`/`edlclient` library's response-parsing code, not
specific to this device. Recovered by simply retrying after the device
re-enumerated; if it recurs, check whether it correlates with a specific
response type from a device that's been through several interrupted
Sahara/firehose sessions in a row.

## Phase 3: WiFi (WCNSS/Pronto) — in progress, hardware crash found and isolated

**Partition-mapping bug found and fixed before any WiFi code ran**: earlier
research (this session, before hardware testing) assumed `persist` was
`mmcblk0p21` based on the firmware package's rawprogram XML. Parsing this
*specific unit's* real GPT directly (`dd`-ing the first 66 sectors of
`/dev/mmcblk0` over the working SSH link and decoding the partition entries
in Python) showed `persist` is actually **`mmcblk0p17`** (65536 sectors /
32 MiB); `p21` is `fsg`. `boot`/`misc`/`recovery`/`userdata` (10/20/19/25),
already in use since Phase 1/2, were cross-checked against the same GPT
parse and are correct — only the WiFi-specific `persist` research was
wrong. **Lesson**: partition-name-to-index mappings from firmware-package
metadata are not reliable across units/releases; parse the real on-device
GPT before trusting an index.

**This unit's real, factory-written WiFi calibration is intact**: mounting
the real `persist` (read-only) showed a genuine `WCNSS_qcom_wlan_nv.bin`
(29816 bytes, dated Dec 2014, `sha256:5ccdc4d3b3aa3a7a1690ad98dacaacc48a786afac6b16ca994a63514709c2819`)
already present — this is this specific board's own factory calibration,
never touched by any work this session (only `userdata` has ever been
reformatted). This is a better calibration source than the generic
firmware package's `persist.img` blob (a *different* file,
`sha256:ea91b9b40284f31981dce5dd48183cf179d6fc9c2faa10ee4c4a9527088803cb`),
so the WiFi service mounts this unit's real `persist` partition read-only
(never written — the data is irreplaceable) rather than shipping any
calibration blob in the image.

**Stock `wcnss_service` cannot run under Alpine/musl**: it's a dynamically
linked Bionic binary (`interpreter /system/bin/linker`) needing
`libcutils`/`libutils`/`liblog`/`libqmiservices`/`libqmi`/`libqcci_legacy`/
`libqmi_client_qmux`/`libstdc++` — none of which exist in this userspace,
and the QMI libraries talk to Android's modem IPC stack, which has no
equivalent here. Following GM8's own precedent (`gm8_wcnss_trigger.c`) for
the identical problem on its WCNSS chip family: a small custom static
helper (`scripts/vestel_wcnss_trigger.c`, ~4KB static armv7/musl binary)
just opens `/dev/wcnss_wlan` and holds it via `pause()` — this alone is
what triggers the WCNSS subsystem to power up. Confirmed via stock
`init.target.rc`/`init.qcom.rc`: stock Android never writes to
`/sys/module/firmware_class/parameters/path` for this device (unlike
GM8), so the WiFi firmware files under `system.img`'s
`etc/firmware/wlan/prima/` (`WCNSS_cfg.dat`, real, extracted and
sha256-verified against the mounted `system.img`) need no path override.
`WCNSS_qcom_cfg.ini` has no static source anywhere in the firmware dump
(it's an Android-runtime-generated symlink to `/data/misc/wifi/...`) —
resolved by reusing GM8's own copy of this file verbatim, since it's
generic prima-driver-defaults, not per-device data (confirmed by content:
it's the standard Qualcomm WCN "override factory defaults" template, no
device-specific values). `/dev/wcnss_wlan`/`/dev/wcnss_ctrl` misc minors
are dynamically assigned (`10:61`/`10:62` at last check) and must be read
from `/sys/class/misc/wcnss_{wlan,ctrl}/dev` at service-start time, not
hardcoded, since this device has no devtmpfs/mdev.

**Confirmed hardware crash, precisely isolated via incremental on-hardware
testing — root cause not yet found**: with the OpenRC service
(`scripts/vestel-wifi-hw.initd`) wired up and enabled at boot, the board
became completely unresponsive (no ICMP, no SSH, RNDIS gadget still
enumerated at the USB level) some time after boot. Rather than guess from
that one compound-script failure, the sequence was re-tested **one step at
a time** over an already-established SSH session on a rebuilt image with
the WiFi service removed from the `default` runlevel (so nothing
WiFi-related auto-starts):
1. `mount -t ext4 -o ro /dev/mmcblk0p17 /persist` — connectivity survives.
2. `mknod`ing `/dev/wcnss_wlan`/`/dev/wcnss_ctrl` from the sysfs-reported
   minors, plus the `WCNSS_qcom_wlan_nv.bin` symlink — connectivity
   survives.
3. Starting `vestel-wcnss-trigger` (`open("/dev/wcnss_wlan", O_RDWR)` then
   `pause()`) — the process starts, `dmesg` shows `wcnss_wlan triggered by
   userspace` (the kernel driver received and accepted the open), and
   connectivity survives immediately after.
4. **~5 seconds later, with nothing further run** (no `insmod` of
   `pronto_wlan.ko` yet — that step was never reached) — the board went
   completely unresponsive. A power cycle recovered it cleanly (fully
   working SSH/dropbear baseline afterward, no storage corruption, `misc`
   BCB/`boot`/`userdata` all intact) — this is a clean hang/crash, not
   data loss.

This isolates *when* the failure starts (asynchronously, a few seconds
after `open("/dev/wcnss_wlan", ...)` succeeds, before the kernel module is
even loaded) and rules out `vestel_wcnss_trigger.c`, the OpenRC script
logic, and `pronto_wlan.ko` (never reached) as the cause. **It does not
establish *what* fails or *where***: opening the device node is the
isolated trigger, not a confirmed hardware fault. Plausible causes not yet
distinguished include WCNSS firmware loading, a driver/PIL
(subsystem-restart-framework) behavior, a watchdog, or a power-delivery
issue on the Pronto rail — and it isn't yet established whether the board
fully freezes, reboots on its own, or the USB link alone drops while the
SoC keeps running. **Do not re-enable `vestel-wifi-hw` in the `default`
runlevel or manually run `rc-service vestel-wifi-hw start` / start the
trigger binary against real hardware again without capture in place
first**: kernel logs streamed live to a *host-side* file over the existing
SSH session (a device-side logger risks losing its final buffered output
in the same event that kills the connection), a check of whether this
kernel's `dmesg` supports `-w` (or another supported live kernel-log
reader) before relying on it, a plan to check `/proc/last_kmsg` on
recovery in case this kernel/bootloader retains a persistent log region
across a warm reset (unlikely to survive a full PSU power cycle, since
that's a cold boot with no RAM retention — but worth checking after any
recovery that turns out to be a self-reboot rather than a full freeze),
and a human watching the bench PSU's voltage/current display through the
test to record whether current draw drops (power loss), spikes then drops
(a fault/short), or holds steady (SoC still executing). One controlled
reproduction with all of that in place should be enough to tell those
apart, without changing the current (WiFi-disabled) boot configuration.

**Root cause found via one controlled, fully-instrumented reproduction**:
kernel log streamed live to a host-side file over the existing SSH session
(`ssh ... 'cat /proc/kmsg' > hostfile.log &` — `/proc/kmsg` exists on this
kernel even though BusyBox's `dmesg` here has no `-w`; no `/proc/last_kmsg`
or pstore/ramoops is available, so a cold power-cycle would lose the ring
buffer entirely regardless), a 0.5s timestamped ping loop, and
`udevadm monitor -k -u --subsystem-match=usb`, all logging independently
on the host. With this running, the exact same trigger sequence as the
earlier crash was re-issued and watched for 85+ seconds (well past the
~5s window of the original observation) — **ping, kernel log, and USB
events all stayed completely clean the whole time**; the earlier
"board unresponsive" conclusion was very likely a misread of one hung
follow-up SSH command (checking `/proc/<pid>/fd` immediately after
starting the trigger) rather than a real board-wide freeze — a correction
worth recording plainly, not glossed over. Checking the trigger process
directly found the actual, precise mechanism:
```
State:	D (disk sleep)
```
(`/proc/<pid>/status`, i.e. uninterruptible sleep — **not killable, not
even with `SIGKILL`**, though harmless to the rest of the system: dropbear,
networking, and unrelated shell commands kept working normally for the
30+ minutes the stuck process was observed), stack showing
`_request_firmware_load`, and `/sys/class/firmware/fb21b000.qcom,pronto/
loading` reading `0` — a **pending userspace-firmware-loading-helper
request that will never be serviced**, since this device deliberately has
no `mdev`/`udev` daemon running (removed in Phase 2 because this kernel's
`devfs`/`mdev` `mount -t devtmpfs` fails and shadows `/init`'s persistent
`/dev` nodes with a fresh tmpfs — see Phase 2 above). The kernel's
in-kernel direct `request_firmware()` filesystem lookup falls back to this
(broken, unfixable-without-udev) legacy helper path when it doesn't find
the firmware at its own default search location. **The actual bug**:
the firmware files were placed at `/etc/firmware/wlan/prima/` — mirroring
where they live *inside* `system.img`, which mounts at `/system` on a real
Android device (i.e. the real absolute stock path is
`/system/etc/firmware/wlan/prima/`) — while this Alpine root's `/lib/
firmware/` (the kernel's actual default direct-lookup path) exists but is
empty. **This is a path bug, not a hardware or firmware fault** — the fix
is to also place (or symlink) the firmware files under `/lib/firmware/
wlan/prima/` so the direct in-kernel lookup succeeds and the broken
userspace-helper fallback is never reached.

**Fix verified live (cheap SSH-only retest, no EDL flash needed for this
part)**: copied the same files to `/lib/firmware/wlan/prima/` on the
already-running v9 device and started a fresh trigger against a new
`/dev/wcnss_wlan` node, with the same live kernel-log/ping/USB-event
capture running throughout. The new trigger process went straight to
`State: S (sleeping)` (not `D`) — the config-file firmware load now
succeeds immediately — and `/sys/class/firmware/` no longer shows a
pending request. Connectivity never wavered. **The original stuck D-state
process from the first reproduction cleared on its own** at some point
during this investigation (no longer present in `ps` afterward) —
apparently not permanent, though still unkillable by signal while stuck.

**A second, separate missing-firmware bug found immediately after, by the
same method**: with the config-file load now fixed, `dmesg` revealed
`pil_pronto fb21b000.qcom,pronto: wcnss: Failed to locate wcnss.mdt`
followed by `wcnss_wlan ...: Peripheral Loader failed on WCNSS.` — a
**second, entirely different firmware file**: the actual Pronto/WCNSS
Peripheral Image Loader (PIL) firmware image, not the WLAN driver's
config files. Critically, **this failure is clean and graceful, not a
hang** — the driver logs the error and returns, no D-state, no stuck
process, connectivity unaffected throughout. This is one plausible
explanation for why `subsys2/state` never left `OFFLINE` in every test
this session. The firmware dump's top-level `wcnss.mbn` (4,032,408 bytes,
sha256 `1a1579ab1106ef892b733dd99f26927312d9e58967b38106650926c3873e7ad4`)
is a genuine, self-contained ELF PIL image (`readelf -l` shows 10 program
headers with real in-file segment data, not stub references to external
`.bNN` files — i.e. not the split `.mdt`+`.bNN` scheme, a single
self-contained image) — copied to the live device as `/lib/firmware/
wcnss.mdt` (byte-verified) and to the persistent rootfs build at the same
path. **A fresh trigger attempt after adding this file did not reproduce
the failure message at all** — no new WCNSS-related `dmesg` output
appeared — most likely because the driver's PIL/subsystem-restart state
machine only attempts a boot once (or with backoff) per kernel lifetime
and had already recorded two failures earlier in the same boot; a
**genuinely fresh boot is needed to know whether `wcnss.mdt` actually
fixes PIL loading**, since re-opening `/dev/wcnss_wlan` on the same
long-running kernel instance did not retrigger the attempt.

**A "real, total hang" was observed on the next fresh-boot test — and
later shown to very likely be the same host-side false alarm, not a
device crash.** With both firmware fixes in place, a fresh trigger showed
`dmesg` stop dead right after `wcnss_wlan triggered by userspace`, and an
independent host-side ping loop went from continuous replies to 100%
loss with no recovery. This was initially treated as a genuine
unrecoverable kernel hang and used as grounds to pause live testing. Two
things undermined that conclusion once found:
1. **The host's RNDIS interface (`enp15s0f3u1`) turned out to be managed
   by NetworkManager**, which silently resets/reflushes the interface's
   address independently of anything happening on the device — confirmed
   later the same session as the cause of *other* "board unresponsive"
   false alarms (`nmcli device status` showed it as disconnected
   under NM control; fixed permanently with
   `sudo nmcli device set enp15s0f3u1 managed no`). The "hung" ping loop
   used the same long-lived process/IP the whole test, exactly the setup
   this NM behavior would silently break mid-test with no on-device
   signal at all — a scenario never actually distinguished from a true
   device hang at the time (that specific host `ip addr show` check
   wasn't run in the moment).
2. **A subsequent, differently-instrumented reproduction later the same
   session — with a passive debugfs clock/subsystem poll added alongside
   the kernel-log and ping capture — reached the exact same point
   (`wcnss_wlan triggered by userspace`, then silence) with the board
   staying fully SSH-reachable throughout**, no hang at all; the stuck
   process was simply back in the same `D`-state
   `_request_firmware_load` wait, this time for `wcnss.mdt` itself
   despite the file existing at the right path.

**The real explanation, found via `/sys/class/firmware/<dev>/loading`
still reading `0` (pending) for `wcnss.mdt` despite the file being
present and correctly placed**: large firmware images on this kernel
apparently don't reliably resolve through the direct in-kernel
filesystem lookup at all — they fall through to the same broken
userspace-helper path as before, regardless of file placement. Manually
servicing that one pending request by hand (`echo 1 > loading; cat
wcnss.mdt > data; echo 0 > loading`) unblocked it instantly and produced
real forward progress never seen before: `pil_pronto ...: wcnss: loading
from 0x0d200000 to 0x0d80f000`. That in turn revealed the actual, final
bug: `pil_pronto ...: wcnss: Failed to locate blob wcnss.b02 or blob is
too big` — **the PIL loader uses the classic split scheme after all**:
`wcnss.mdt` is only a 732-byte metadata/hash header, and the real
segment payloads live in separate `wcnss.b00`, `.b01`, `.b02`, `.b04`,
`.b06`, `.b07`, `.b08`, `.b09` files (`.b03`/`.b05` are correctly absent
— they correspond to the two zero-filesize `LOAD` segments in the ELF
header, needing no blob). **The standalone 4MB `wcnss.mbn` found earlier
in the firmware package's top level was simply the wrong file** — a
different, unrelated single image, not this device's real PIL metadata
header at all. The genuine, complete set (`wcnss.mdt` + all 8 `.bNN`
blobs) was found inside `NON-HLOS.bin` — the actual `modem` GPT partition
image (mounted at `/firmware` by stock Android; same "firmware lives on
a separate partition" pattern as GM8's `modem_a`) — under `image/`,
alongside `modem.mdt`+`.bNN`, `adsp.mdt`+`.bNN`, and several DRM
component images. All 9 files (`wcnss.mdt` + 8 blobs) are byte-verified
against that source and preserved in
`backups/phase3-wifi-working/firmware/`.

**Fix**: a permanent minimal userspace firmware-loading helper
(`scripts/vestel_fw_helper.c`, compiled as `vestel-fw-helper`) — this
device has no `mdev`/`udev` at all (see Phase 2), so nothing was ever
going to service the kernel's classic userspace-helper fallback
protocol for *any* firmware request, however file placement was
arranged. The helper listens on the kernel's uevent netlink socket for
`SUBSYSTEM=firmware ACTION=add` events and services each one from
`/lib/firmware/`. **First version had a real bug of its own**, found via
a very literal error message (`Blob size 4096 doesn't match 12596`):
the kernel's firmware `data` sysfs `bin_attribute` write is capped at
`PAGE_SIZE` (4096 bytes) *per `write(2)` syscall* regardless of how large
a buffer userspace passes in one call, and the first version neither
chunked its writes nor checked their return value, so every blob over
4KB was silently truncated. Fixed by looping on `write()`'s actual
return value until the whole buffer is sent (the destination fd's
position auto-advances like any regular file). `vestel-wifi-hw.initd`
starts this helper first, before mounting persist or touching any WCNSS
device node.

**Fully confirmed working, three separate times, including two
independent cold boots with zero manual steps**: `subsys2/state` reaches
`ONLINE` (kernel log shows the complete real sequence — `Brought out of
reset`, `Power/Clock ready interrupt received`, `NV download`, `NV bin
size: 29812` matching this unit's real calibration blob, clean
`wcnss_post_bootup` completion), `pronto_wlan.ko` loads cleanly
(`wlan: driver loaded`), `wlan0`/`p2p0` appear, and `iw dev wlan0 scan`
returns real nearby access points
(`SUPERONLINE_WiFi_C8AF`, `SUPERONLINE_Wi-Fi_8C28`, `dlink`, `Ersan1`,
...) — the same acceptance bar GM8 used. `vestel-wifi-hw` is enabled in
`/etc/runlevels/default/` on both the live device and the build tree; a
freshly-built, byte-verified image (v12) was flashed via EDL as the
final, reproducible artifact, replacing the accumulated live-SSH-patch
state, and preserved at
`backups/phase3-wifi-working/userdata_v12_wifi_working.img`. Board
health (dropbear, ping, unrelated shell commands) never wavered during
any of the successful runs. USB SSH remained available throughout, as
required.

**WPA association, DHCP, and internet — verified end-to-end, real
credentials, following GM8's exact security convention**: the protected
config lives at `/etc/vestel-wpa_supplicant.conf` (mode `0600`,
root-owned), symlinked from `/etc/wpa_supplicant/wpa_supplicant.conf` —
**created directly on the device over SSH, never committed to this repo
and never baked into any built image** (confirmed by grepping the built
`userdata_v13_with_dhcp.img` binary for the credentials — absent, as
expected, since `ROOTFS`/`STAGE` never contain this file). Two real,
separate bugs found and fixed getting here:
- `wpa_supplicant`'s own `find_wireless()` auto-detection picked `p2p0`
  (also present alongside `wlan0` on this multi-vif WCNSS driver) instead
  of the real station interface — association looked like it worked
  (`wpa_state=COMPLETED` briefly observed on `p2p0`) but no real station
  traffic could ever flow. Fixed by setting `wpa_supplicant_if="wlan0"`
  in `/etc/conf.d/wpa_supplicant`, forcing the correct interface
  explicitly rather than relying on auto-detection.
- The first network tried failed every time with an extremely fast
  (~8-10ms) connect-then-disassociate loop, reason code 2. Foreground
  `wpa_supplicant -d` debug output was unambiguous:
  `4-Way Handshake failed - pre-shared key may be incorrect` /
  `CTRL-EVENT-SSID-TEMP-DISABLED ... reason=WRONG_KEY` — i.e. this was a
  real credential/AP-specific issue for that one network, not a driver or
  WCNSS bug (a different network's credentials associated cleanly on the
  first try, `wpa_state=COMPLETED` with a stable link, confirming the
  mechanism itself was already correct).
- A `local.d` script (`scripts/vestel-wlan-dhcp.start`, same convention as
  `vestel-usb-net.start`) runs `udhcpc -i wlan0 -b` at boot — busybox's
  background/retry mode, so it doesn't need to race `wpa_supplicant`'s own
  association timing; it just keeps retrying (harmlessly logging failed
  attempts to `/var/log/vestel-wlan-dhcp.log`) until `wlan0` actually
  associates, then obtains a real lease automatically.

Confirmed on a freshly-flashed image (v13, credentials recreated over SSH
immediately after flashing, per the never-baked-in design): fully
automatic from cold boot, `wpa_state=COMPLETED`, a real DHCP-assigned
IPv4 address, and a real `ping -I wlan0 1.1.1.1` reaching the open
internet — the complete GM8-style acceptance bar (association, DHCP,
internet) now met on this device too. `userdata_v13_with_dhcp.img` (no
credentials, as verified) is preserved at
`backups/phase3-wifi-working/userdata_v13_with_dhcp.img`. **To
(re)configure WiFi on this device**: SSH in and write
`/etc/vestel-wpa_supplicant.conf` (see the `network={ ssid=... psk=...
key_mgmt=WPA-PSK }` block format above), `chmod 600` it, then
`rc-service wpa_supplicant restart` — never add real credentials to this
repository or to any image build.

**Two more real, previously-undiscovered bugs found immediately after,
via genuine interactive use (everything up to this point had only ever
used SSH for non-interactive remote commands, which never exercises
either of these):**
- **Interactive SSH logins failed** (`PTY allocation request failed` /
  `shell request failed`, even though key-based auth succeeded) —
  dropbear's own debug log (`dropbear -F -E -p <port>`) was unambiguous:
  `open /dev/tty failed - could not set controlling tty: No such file or
  directory`. `/dev/pts` was never mounted at all (no `mdev`/`udev` means
  nothing else was ever going to do it — OpenRC's own `sysfs` service
  only handles `/sys`-side pseudo-filesystems, confirmed by reading its
  source), and `/dev/tty` itself had been deliberately left out of
  `/init`'s device list earlier this session over an *old, since-obsolete*
  theory that it caused "complete USB silence" — the real cause of that
  turned out to be the unrelated devfs/mdev tmpfs-shadowing bug, already
  fixed. Fixed with a new `boot`-runlevel service
  (`scripts/vestel-devpts.initd`, ordered `before dropbear`) that mounts
  `devpts`, plus a plain `mknod /dev/tty c 5 0` on the real root (persists
  like every other device node this session). Confirmed working by an interactive login.
- **`apk add` couldn't find any package** (`(no such package)`, `OK: 95
  distinct packages available` — a tiny stale local cache) — `/etc/apk/
  repositories` had simply never been created on the device; the build
  process only ever passed repo URLs as one-off `--repository` flags to
  `apk`, never wrote them to a persistent config file. Fixed by writing
  the same three edge repo URLs used during the build
  (`main`/`community`/`testing`) to `/etc/apk/repositories`, confirmed
  jumping to `33911 distinct packages available` and a real package
  (`htop`) installing and running cleanly — this time with no
  `--no-scripts` workaround needed at all, confirming that requirement
  was specific to the QEMU-user-mode build environment, not a real
  hardware limitation.

**Optional root password / password-auth SSH** (once the device is on a Wi-Fi network rather than only an isolated USB link, this widens root's exposure if that network is shared, so it is opt-in): `DROPBEAR_OPTS=""` in `/etc/conf.d/dropbear` (was `"-s"`, key-only)
enables password auth alongside the existing key, and a root password was
set via `chpasswd`. **The password itself, like WiFi credentials, is
never baked into any build or committed anywhere** — `ROOTFS`'s
`/etc/shadow` keeps root's password field empty by design; only the
*policy* (`DROPBEAR_OPTS=""`) persists in the build tree. Re-set the
password the same way after any fresh flash: `echo "root:<password>" |
chpasswd` over SSH.

**Lesson worth stating plainly**: across this whole investigation, most
of the apparent "board crashes" that triggered a recovery power-cycle
were real (the original D-state/missing-firmware findings, verified with
independent host-side capture), but at least one — the one that
originally justified pausing live testing pending UART access — was very
likely this same host-side NetworkManager artifact, not a device fault.
The distinguishing check (`ip addr show <iface>` for a live `inet` line)
is cheap and should run *before* concluding a device hang, every time,
not just after being burned by it twice already.

**Operational gotcha (host-side, not a device bug, recurs across host
reboots too — re-apply each time)**: the host's RNDIS interface
(`enp15s0f3u1`) gets a new kernel ifindex on some re-enumerations (a
device reset/reboot, or a host reboot), silently dropping any
statically-assigned IP, and NetworkManager will re-claim and manage a
newly-created interface object even after being told `managed no` for
a previous instance of the same-named device. Before concluding the
*board* is unresponsive: `sudo nmcli device set enp15s0f3u1 managed no`,
then `sudo ip addr add 172.16.42.2/24 dev enp15s0f3u1`, then retest.

**Root filesystem grown to fill the real partition (post-Phase-3
housekeeping)**: every `userdata` image since Phase 1 was deliberately
built at a fixed 1 GiB (262144 × 4096-byte blocks) — far smaller than the
real `userdata` GPT partition (`mmcblk0p25`, 25554911 sectors ≈ 12.2 GB)
— specifically to keep EDL flash-cycle turnaround fast across the dozens
of iterations this bring-up needed. With that iteration phase done, the
live filesystem was grown online to fill the real partition: `apk add
e2fsprogs e2fsprogs-extra` (not part of the minimal package set), then
`mknod /dev/userdata_blk b 179 25` (the node `/init` uses during the
ramdisk-pivot mount doesn't exist in the *final* root's `/dev` — it's
transient to the pivot itself) followed by `resize2fs /dev/userdata_blk`
— a fully standard, safe online-grow operation given `resize_inode` was
already present in every build's `mkfs.ext4` flags specifically for this.
Confirmed: `df -h /` went from `974.7M` to `12.1G` with no service
interruption (SSH, WiFi all confirmed still healthy immediately after).
**This is a property of the live filesystem on the physical device, not
something the build tree captures** — a future image rebuilt from
`ROOTFS`/`STAGE` and freshly flashed will start back at the small 1 GiB
size (deliberately — that build-time size was never meant to be
permanent) and need this same two-command sequence repeated once after
flashing, until/unless the `mkfs.ext4` invocation used for building is
itself changed to target the full partition size directly.
