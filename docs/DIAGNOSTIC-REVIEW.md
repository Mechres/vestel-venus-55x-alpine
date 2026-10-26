# Independent diagnostic review — 2026-09-08

The current tests do **not** establish that the bootloader rejects custom
ramdisks or that `/init` never executes. There are confirmed defects in the
diagnostic probes. Fix and validate the probes before pursuing aboot changes.
The original cause of the earlier silent boots remains unproven. This review
used local artifacts and the recorded commands; no phone was flashed,
reset, or accessed during the review.

## Confirmed findings

### 1. Test #4 cannot create its marker node in the shipped filesystem

`vestel_minimal_boot.img` contains exactly one cpio entry, `init`, mode 0755.
Its `init_direct.c` calls:

```c
mknod("/dev/misc_dbg", S_IFBLK | 0660, makedev(179, 20));
int fd = open("/dev/misc_dbg", O_WRONLY);
```

There is no `/dev` directory in this archive, and the program never creates
one. The stock kernel has `CONFIG_INITRAMFS_SOURCE=""` and
`# CONFIG_DEVTMPFS is not set`. This is not a setup where automatic device
population can be assumed. Missing-parent `mknod` returns ENOENT (also
reproduced locally without accessing a block device). The program ignores
that error, skips the write when open fails, then pauses forever. A missing
marker is therefore entirely consistent with `/init` executing successfully.
Adding the directory alone does not validate storage readiness or persistence.

### 2. The minimal emulation test did not validate this write

The recorded `localtest3` setup created `rootfs/dev`, unlike the actual
ramdisk, and copied the loop block node to `/dev/misc_dbg_real`. The binary
instead creates and opens `/dev/misc_dbg` with hardcoded device 179:20.
There is no redirection to the loop node. QEMU user mode does not magically
map that major/minor to the loop device. The test timed out; a live process
in the unconditional pause loop proves neither open nor write succeeded.

The `localtest2` pivot test did produce pre/post-pivot log lines, but used a
modified script and the host kernel's filesystems. It was useful userspace
testing, not validation of the exact boot image on the vendor kernel.
[QEMU documents the user-mode scope here](https://www.qemu.org/docs/master/user/main.html).

### 3. devtmpfs is definitely unavailable

Extracted the embedded IKCONFIG from the stock boot kernel, rather than
guessing from Android's age. Relevant values:

```text
CONFIG_IKCONFIG=y
# CONFIG_IKCONFIG_PROC is not set
CONFIG_INITRAMFS_SOURCE=""
CONFIG_BLK_DEV_INITRD=y
CONFIG_RD_GZIP=y
CONFIG_BINFMT_ELF=y
# CONFIG_DEVTMPFS is not set
CONFIG_TMPFS=y
CONFIG_EXT4_FS=y
CONFIG_MMC_BLOCK_MINORS=32
CONFIG_AEABI=y
CONFIG_VFP=y
CONFIG_VFPv3=y
CONFIG_ANDROID_RAM_CONSOLE=y
# CONFIG_PSTORE is not set
```

Every `mount -t devtmpfs devtmpfs /dev` in the proposed init must fail on
this kernel. Use tmpfs plus explicit device nodes or a userspace device
manager. Mount tmpfs **before** creating nodes, otherwise it hides them.
Provide real `/dev/null` (character 1:3), `/dev/console` (5:1), and needed
block devices. Discover block major/minor values from sysfs and wait for
device registration with a bounded retry; node existence alone is insufficient.

This defect does not, by itself, explain the first missing marker in tests
that manually create a block node before attempting devtmpfs. Keep that
distinction when interpreting results.

### 4. Missing CONFIG_BINFMT_SCRIPT is not evidence of missing shebang support

Upstream Linux **3.4** builds `binfmt_script.o` unconditionally; it is not
controlled by the later CONFIG_BINFMT_SCRIPT option. See the
[version-specific fs/Makefile](https://raw.githubusercontent.com/torvalds/linux/v3.4/fs/Makefile).
The claim in `init_wrapper.c` that kernels of this age commonly omit script
support is not substantiated for this board. Vendor changes remain possible;
test direct execution of a tiny shebang script from recovery to settle it.
Invoking `busybox sh script` would bypass the kernel's shebang loader and
would not answer that question.

### 5. Marker failures and durability were not checked

The shell probes discard dd errors and do not flush the markers explicitly.
The C probe ignores mknod, seek, write, and close results and never calls
fsync. All can fail independently of whether PID 1 started. A reset or power
cut also makes an unflushed marker unreliable. In shell variants, the first
write depends on several commands and a pipeline, not the first instruction.

Offset 64 in Android's bootloader-message structure is the beginning of the
recovery field, not generic scratch storage. Do not keep treating misc as a
free debug partition; prefer a diagnostic file in the already disposable
userdata filesystem, with a separate live/logging channel for mount failures.

## Next steps

1. Re-establish the already proven patched-recovery ADB environment. Save
   current boot/misc readbacks and identify their hashes before changing
   state. Keep the full eMMC backup and known-good recovery artifact intact.
   Verify any subsequent flashed image by readback, not merely a successful
   host-side repack or EDL message. The most recent local minimal image has
   not been independently verified as the phone's current contents.
2. In that working recovery shell, capture `/proc/cmdline`, `/proc/filesystems`,
   `/proc/partitions`, relevant `/sys/class/block/*/dev`, and dmesg. Check
   `/proc/last_kmsg` after a failed boot followed by a warm recovery boot:
   RAM console is configured, although retention and exposure must be tested.
   A complete power loss may destroy that evidence.
3. Run each candidate binary as a normal child process from recovery first.
   Give diagnostic programs a finite self-test mode that reports every errno
   and exits; do not accept a pause loop as success. Test direct shebang
   execution separately. Test mounting the existing 128 MB userdata test
   filesystem and reading/writing/fsyncing a canary there on the actual kernel.
   These tests need no new replacement `/init` flash.
4. Use the working recovery ramdisk as a controlled starting point: retain
   Android init, ueventd, and ADB while running a one-shot diagnostic service.
   This preserves a channel for errors while proving the same binary and
   mount logic. It is not yet a PID-1/pivot test.
5. Only after those checks pass, attempt a custom PID 1 with correct /dev
   setup, checked calls, bounded storage retries, persistent successful
   writes, and visible error reporting. Keep PID 1 alive on failure. Mount
   proc/sysfs and move required mounts into the new root before switch_root;
   provide a valid long-lived init there. Compare one change per boot.
6. If a validated probe still fails only as PID 1, investigate the actual
   kernel log, selected boot partition, loaded image, ELF compatibility,
   watchdog/power behavior, and bootloader loading in that order as evidence
   directs. Do not label aboot rejection as established from marker absence.
