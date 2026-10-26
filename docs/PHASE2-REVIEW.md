# Phase 2 review — 2026-09-08

The hardware hang has not yet been located. The saved evidence establishes
the pivot, but **does not establish successful exec of Alpine**. USB is also
configured too late to distinguish an early boot failure from a service
stall. Fix those two observation gaps before another rootfs redesign.

Reviewed the checksummed images under `backups/phase2-alpine-attempt/`, the
actual files inside the ext4 image using read-only debugfs, the preserved
misc trace, production C source, and the recorded local-test commands/logs.
Both image checksums pass. No phone access, reset, or flashing was performed.

## Findings

1. **The exec failure logger is broken.** `init_final_v3_source.c` closes
   `g_misc_fd` immediately before execve, but leaves the integer nonnegative.
   If execve fails, its `if (g_misc_fd < 0)` branch does not reopen anything;
   misclog then writes to the closed descriptor. Even reopening the old
   pathname would fail: `/dev/misc_dbg` was created in the old ramdisk, not
   in the Alpine root. The saved offset-896 marker says
   `BOOT-PIVOT-OK-EXECING-REAL-INIT`, written **before** exec. Empty offset
   960 does not prove success. Source is now corrected to open with
   O_CLOEXEC and retain the descriptor until exec: failure can still log,
   success closes it automatically. Preserved images are unchanged.

2. **USB is downstream of every other default service.** The image's
   `/etc/init.d/local` has `depend() { after *; ... }`. The USB script lives
   in `/etc/local.d`, while sshd is also in default. A blocking SSH startup,
   or an earlier boot service, prevents the USB script from being reached.
   Thus no gadget enumeration is not evidence of an early Alpine failure.
   The USB script itself does not fail fast and can return zero despite
   failed gadget writes; OpenRC's local service explicitly returns zero
   even when a `.start` script fails.

3. **The local verification skips important paths.** The saved commands
   pre-mount `/proc`, `/sys`, and `/run`, then invoke `/sbin/openrc`
   directly. They never start BusyBox init or exercise its inittab actions.
   They also skip OpenRC's initial proc/run mounts on the phone. The real
   `/usr/libexec/rc/sh/init.sh` does mount proc/run, so merely observing that
   the C init does not mount proc is *not* a demonstrated root cause.
   `/tmp/boot3.log` contains missing-module-directory messages and
   `Failed to set the system clock`, despite the reported success. Exit
   zero is not equivalent to an error-free boot. The saved sysinit log
   explicitly identifies the host kernel as Linux 7.2.2-1-cachyos.

4. **Compatibility tests were narrower than the installed image.** The
   image actually contains BusyBox 1.38.0-r4 (dynamic), musl 1.2.6-r3,
   OpenRC 0.63.2-r0, OpenSSH 10.5_p1-r1 and libcrypto 3.5.8-r0. The earlier
   hardware smoke test used BusyBox *static* 1.37.0-r31 and musl -r2.
   Running a version command also does not exercise SSH startup or entropy
   acquisition. These differences are not proof of incompatibility, but
   the exact installed binaries must be the ones tested.

5. **Device-node and SSH explanations still need checks.** The production
   source ignores all five new character-device mknod return values, then
   logs a hardcoded success marker. Validate each node's type, major/minor,
   permissions, and operation. A connection reset before the SSH banner
   does not specifically prove missing urandom. SSH foreground stderr is
   needed. `/dev/pts` also needs a devpts mount for interactive SSH later;
   ptmx alone does not supply a PTY filesystem.

## Leading service-stall hypothesis: entropy before USB

The new code creates `/dev/random` as well as `/dev/urandom`. OpenSSL's
Linux entropy fallback can wait for `/dev/random` readiness on old kernels
when getrandom/getentropy are unavailable. Its wait has no timeout. A
fresh boot on a bare board differs materially from a mature host kernel or
recovery after substantial device activity. With `local` ordered after
sshd, an entropy wait during SSH startup can keep USB entirely absent.
See the [OpenSSL 3.5 entropy implementation](https://raw.githubusercontent.com/openssl/openssl/openssl-3.5.0/providers/implementations/rands/seeding/rand_unix.c).
The image's libcrypto imports getentropy/shmget and contains both random
device paths, consistent with this possibility; that is not a captured
hardware backtrace. **This is a hypothesis, not a confirmed diagnosis.**

Do not bypass cryptographic entropy checks or alias random to urandom to
make boot pass. First identify the waiting process/syscall. If confirmed,
use a verified entropy source and keep USB setup independent of SSH.

## Concrete next experiment

Keep one test image, changing only observation and USB ordering as needed:

1. Before rebuilding, use recovery to inspect the failed boot's actual
   userdata logs (`vestel-boot.log`, `vestel-usb-net.log`, `dmesg`, messages),
   and collect last_kmsg if retained. The preserved userdata image has no
   `vestel-boot.log` and an empty /dev, so it is not evidence of runtime
   filesystem changes despite the `on_device` filename. Distinguish the
   flashed input from a post-boot readback. Do not overwrite it with a new
   build before collecting the failed boot's logs.
2. Rebuild C init with the O_CLOEXEC fix in `scripts/init_final.c`.
   Keep the existing checked pivot unchanged. Clear/identify the new trace
   before the test and verify the flashed bytes as usual.
3. Stage `scripts/vestel-openrc-trace` at `/sbin/vestel-openrc-trace`, mode
   0755, and use `scripts/inittab.phase2-diagnostic` as `/etc/inittab` in a
   copy of the rootfs. It logs entry **before** OpenRC is called, captures
   output and runlevel return codes, periodically syncs while waiting, and
   records process wait states plus entropy count. The trace goes to
   `/var/log/vestel-openrc-trace.log`, so collect it through recovery after
   failure. It is temporary diagnostic instrumentation, not a final init.
4. Inspect the first incomplete checkpoint. No ENTER means investigate
   exec, dynamic loader, BusyBox init, inittab, and stdio before OpenRC.
   ENTER without EXIT identifies a runlevel to inspect. A waiting sshd or
   ssh-keygen with random-related wait state supports the entropy hypothesis;
   get a native strace or fd/syscall mapping to confirm. A completed default
   with failed gadget writes redirects investigation to the USB script.
5. For useful early access, configure USB immediately after successful
   sysinit/sysfs setup, **before default/sshd**, or make a dedicated OpenRC
   service ordered before sshd. Verify gadget-write results separately from
   IP assignment and do not also run the old local.d copy. USB enumeration
   must not depend on cryptographic service startup. A finite diagnostic
   test with sshd omitted from default is another way to isolate that path.

Use stable stdin and persistent stdout/stderr descriptors for the init
handoff if the new wrapper is not reached. Removing console nodes based
solely on silence was not a demonstrated fix: creating a character node
does not itself perform a blocking read. BusyBox init's console handling
and inittab dispatch are separate from directly running OpenRC; see
[BusyBox init source](https://github.com/brgl/busybox/blob/master/init/init.c).

## Validation performed for this review

- Verified both preserved SHA256 values and inspected actual in-image
  inittab, OpenRC scripts, package versions, and directories.
- Queried the extracted BusyBox configuration: INIT and FEATURE_USE_INITTAB
  are enabled. Missing init-applet support is not the explanation.
- Built corrected C init as static ARM using the existing Zig target.
- Executed a host harness using the production logging code: failed exec
  writes ENOENT at offset 960; successful exec closes the O_CLOEXEC fd.
- Checked the diagnostic wrapper syntax using the image's ARM BusyBox.
- Ran that exact BusyBox init as PID 1 in isolated host PID/mount/network
  namespaces with the diagnostic inittab and a deliberately delayed mock
  OpenRC returning 7. All three ENTER/WATCH/EXIT checkpoints were recorded
  with rc=7, and the namespace was forcibly terminated afterward. This
  validates the instrumentation and inittab path, not the vendor kernel or
  real OpenRC services. Output is in
  `/tmp/vestel-phase2-review/pid1-test/var/log/vestel-openrc-trace.log`.

No new image has been deployed. Hardware confirmation remains required.
