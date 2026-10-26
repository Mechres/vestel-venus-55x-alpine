/* Production ramdisk-pivot /init for the Vestel Venus 5.5x Alpine bring-up.
 *
 * Mount+pivot logic is unchanged from the fully-instrumented probe that
 * was verified end-to-end on real hardware (checked syscalls, bounded
 * 10s retries on both open() and the userdata mount() itself, distinct
 * pre/post-pivot markers, PID/proc-mounts confirmation). On success this
 * version execve()s into busybox's real "init" applet (argv[0]="init"),
 * which reads /etc/inittab and runs the real Alpine/OpenRC boot sequence
 * -- rather than idling with diagnostic markers, which was only ever the
 * probe's terminal state for isolating the pivot mechanism itself.
 *
 * On any failure, falls back to the exact same diagnostic-marker + idle
 * behavior as the probe, so a bad boot is still debuggable via the misc
 * partition without a live console.
 */
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/mount.h>
#include <sys/ioctl.h>
#include <linux/random.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <time.h>

#define MISC_MAJOR 179
#define MISC_MINOR 20
#define UD_MAJOR 179
#define UD_MINOR 25
#define RETRY_MAX 200      /* 200 * 50ms = 10s bounded wait */

static int g_misc_fd = -1;

static void misclog(off_t off, const char *s) {
    if (g_misc_fd < 0) return;
    char buf[64];
    memset(buf, ' ', sizeof(buf));
    size_t n = strlen(s);
    if (n > sizeof(buf)) n = sizeof(buf);
    memcpy(buf, s, n);
    lseek(g_misc_fd, off, SEEK_SET);
    write(g_misc_fd, buf, sizeof(buf));
    fsync(g_misc_fd);
}

static void fmt3(char *out, const char *label, long a, long b, long c) {
    int p = 0;
    for (const char *s = label; *s && p < 32; s++) out[p++] = *s;
    out[p++] = '=';
    long vals[3] = {a, b, c};
    for (int i = 0; i < 3; i++) {
        if (i) out[p++] = ',';
        long v = vals[i];
        if (v < 0) { out[p++] = '-'; v = -v; }
        char digits[12]; int d = 0;
        if (v == 0) digits[d++] = '0';
        while (v > 0 && d < 12) { digits[d++] = '0' + (v % 10); v /= 10; }
        while (d > 0) out[p++] = digits[--d];
    }
    out[p] = 0;
}

static void logstep(off_t off, const char *label, long a, long b, long c) {
    char buf[64];
    fmt3(buf, label, a, b, c);
    misclog(off, buf);
}

static void sleep_ms(long ms) {
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&ts, 0);
}

static void idle_forever(void) {
    for (;;) {
        struct timespec ts = {1, 0};
        nanosleep(&ts, 0);
    }
}

int main(void) {
    /* Confirmed via recovery inspection: mknod's requested mode (0666 on
     * the entropy/null devices below) was silently masked down to 0644
     * by the default process umask (never set here), leaving them
     * non-world-writable/readable as actually needed. sshd's privilege-
     * separated child runs unprivileged and needs full access to these
     * -- this was found by checking the *already-flashed* device's real
     * file modes directly, not inferred. */
    umask(0);

    int r_mkdir = mkdir("/dev", 0755);
    int r_mknod_misc = mknod("/dev/misc_dbg", S_IFBLK | 0660, makedev(MISC_MAJOR, MISC_MINOR));
    int e_mknod_misc = (r_mknod_misc < 0) ? errno : 0;

    int tries;
    int e_open_misc = 0;
    for (tries = 1; tries <= RETRY_MAX; tries++) {
        /* Keep the diagnostic fd usable if execve fails. The kernel closes
         * it only on successful exec, even after chroot hides its pathname. */
        g_misc_fd = open("/dev/misc_dbg", O_WRONLY | O_CLOEXEC);
        if (g_misc_fd >= 0) break;
        e_open_misc = errno;
        sleep_ms(50);
    }
    logstep(64, "S1-mkdir+mknodmisc+errno", r_mkdir, r_mknod_misc, e_mknod_misc);
    logstep(128, "S2-openmisc-tries+fd+errno", tries, g_misc_fd, e_open_misc);

    int r_mknod_ud = mknod("/dev/userdata_blk", S_IFBLK | 0660, makedev(UD_MAJOR, UD_MINOR));
    int e_mknod_ud = (r_mknod_ud < 0) ? errno : 0;
    logstep(192, "S3-mknod-userdata+errno", r_mknod_ud, e_mknod_ud, 0);

    mkdir("/newroot", 0755);
    int r_mount = -1, e_mount = 0, mount_tries;
    for (mount_tries = 1; mount_tries <= RETRY_MAX; mount_tries++) {
        r_mount = mount("/dev/userdata_blk", "/newroot", "ext4", 0, NULL);
        if (r_mount == 0) break;
        e_mount = errno;
        sleep_ms(50);
    }
    logstep(256, "S4-mount-tries+ret+errno", mount_tries, r_mount, e_mount);

    if (r_mount != 0) {
        misclog(832, "BOOT-ABORTED-MOUNT-FAILED");
        idle_forever();
    }

    /* This kernel has no devtmpfs (confirmed absent from /proc/filesystems
     * on real hardware) and mdev's coldplug population isn't proven
     * reliable this early -- explicit, idempotent mknod for the handful
     * of character devices real programs assume exist (sshd in
     * particular needs /dev/urandom for entropy; its absence causes the
     * forked child to die before it even sends its protocol banner,
     * which is the "Connection reset" symptom this addresses). Written
     * once onto the real filesystem, so it persists across every future
     * boot the same way traditional static /dev entries always did. */
    /* NOTE: deliberately NOT creating /dev/console or /dev/tty here.
     * A prior attempt did, and boot went completely silent (not even USB
     * gadget enumeration, which had reliably happened at this same point
     * in every earlier attempt) -- consistent with something opening a
     * real console device and blocking forever on a read, since there is
     * no physical UART/console actually wired up on this board. Keeping
     * only the non-blocking, always-ready entropy/null devices sshd and
     * other programs actually need. */
    mkdir("/newroot/dev", 0755);
    mknod("/newroot/dev/null", S_IFCHR | 0666, makedev(1, 3));
    mknod("/newroot/dev/zero", S_IFCHR | 0666, makedev(1, 5));
    mknod("/newroot/dev/random", S_IFCHR | 0666, makedev(1, 8));
    mknod("/newroot/dev/urandom", S_IFCHR | 0666, makedev(1, 9));
    mknod("/newroot/dev/ptmx", S_IFCHR | 0666, makedev(5, 2));
    /* Explicit chmod, not just relying on umask(0) above -- self-heals
     * nodes already created with the wrong mode by an earlier boot of
     * this exact bug (mknod is a no-op on an already-existing node, so
     * umask alone would not have fixed a device already flashed). */
    chmod("/newroot/dev/null", 0666);
    chmod("/newroot/dev/zero", 0666);
    chmod("/newroot/dev/random", 0666);
    chmod("/newroot/dev/urandom", 0666);
    chmod("/newroot/dev/ptmx", 0666);
    logstep(640, "S4b-essential-devnodes-done", 1, 0, 0);

    /* Confirmed via the ENTER/WATCH/EXIT boot trace on real hardware:
     * "sshd -G" (OpenRC's dependency-check invocation, not the real
     * daemon) sat blocked in poll_schedule_timeout for the entire
     * sysinit runlevel while /proc/sys/kernel/random/entropy_avail
     * climbed slowly (25->32->39->46->53->60 over ~12s) -- this is
     * OpenSSL's RAND_poll heuristic (poll /dev/random for readiness
     * before trusting /dev/urandom), needed because this 3.4 kernel
     * predates getrandom()/getentropy() (added in Linux 3.17).
     *
     * Provenance of the credited bytes, spelled out precisely (per
     * review feedback -- this must not be mistaken for a hardware
     * entropy source): this reads live from /dev/urandom *at this exact
     * boot*, every boot -- never a fixed buffer, never a value stored on
     * disk and replayed. On this pre-CRNG-era kernel (3.4), /dev/urandom
     * is fed by whatever real boot-time jitter (interrupt timing, the
     * cycle counter, etc.) the kernel has gathered into its entropy pool
     * by this point in boot -- genuinely fresh per boot, but on a bare
     * board this early there may be very little of it, so crediting it
     * back does not manufacture new physical randomness; it only
     * unblocks POLL_SCHEDULE_TIMEOUT-style consumers that gate on the
     * entropy_avail counter as a readiness heuristic, trusting the
     * kernel's own pool output for that purpose. This is the same
     * technique the `seedrng` OpenRC service (now also enabled, see
     * `boot` runlevel) uses properly across reboots -- read a fresh
     * seed, credit it, immediately rotate to a new one for next time --
     * except seedrng only runs once OpenRC starts services, i.e. after
     * the exact early-sysinit window this was written to unblock. Keep
     * both: this covers first-boot/pre-OpenRC, seedrng covers every
     * boot after that with proper seed-file rotation. Fine for this
     * device's current scope (a bench-wired lab/dev node on a private
     * USB link); would need a real hardware RNG source, not this
     * self-referential credit, before trusting it for anything
     * long-lived or exposed. */
    {
        int fd_ur = open("/newroot/dev/urandom", O_RDONLY);
        int fd_rand = open("/newroot/dev/random", O_RDWR);
        int r_ioctl = -1;
        int e_ioctl = 0;
        if (fd_ur >= 0 && fd_rand >= 0) {
            struct {
                int entropy_count;
                int buf_size;
                unsigned char buf[256];
            } entropy;
            ssize_t r_read = read(fd_ur, entropy.buf, sizeof(entropy.buf));
            if (r_read > 0) {
                entropy.entropy_count = (int)r_read * 8;
                entropy.buf_size = (int)r_read;
                r_ioctl = ioctl(fd_rand, RNDADDENTROPY, &entropy);
                e_ioctl = (r_ioctl < 0) ? errno : 0;
            }
        }
        if (fd_ur >= 0) close(fd_ur);
        if (fd_rand >= 0) close(fd_rand);
        logstep(704, "S4c-entropy-seed+ioctl+errno", fd_ur, r_ioctl, e_ioctl);
    }

    /* Boot log on userdata itself -- append-only, survives across cold
     * boots, gives a direct way to verify persistence + repeatability
     * (Phase 2's last checklist item) by just counting lines. */
    int fd_log = open("/newroot/var/log/vestel-boot.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd_log < 0) {
        mkdir("/newroot/var", 0755);
        mkdir("/newroot/var/log", 0755);
        fd_log = open("/newroot/var/log/vestel-boot.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
    }
    int e_log_open = (fd_log < 0) ? errno : 0;
    logstep(320, "S5-bootlog-open+fd+errno", 0, fd_log, e_log_open);
    if (fd_log >= 0) {
        const char *line = "BOOT vestel-pivot-init reached pre-exec\n";
        ssize_t r_write = write(fd_log, line, strlen(line));
        int e_write = (r_write < 0) ? errno : 0;
        int r_fsync = fsync(fd_log);
        int e_fsync = (r_fsync < 0) ? errno : 0;
        logstep(384, "S6-bootlog-write+fsync+errno", (long)r_write, r_fsync, e_write ? e_write : e_fsync);
        close(fd_log);
    }
    sync();

    int r_chdir1 = chdir("/newroot");
    int e_chdir1 = (r_chdir1 < 0) ? errno : 0;
    int r_move = mount(".", "/", NULL, MS_MOVE, NULL);
    int e_move = (r_move < 0) ? errno : 0;
    logstep(448, "S7-chdir+msmove+errno", r_chdir1, r_move, e_move);

    int r_chroot = chroot(".");
    int e_chroot = (r_chroot < 0) ? errno : 0;
    int r_chdir2 = chdir("/");
    int e_chdir2 = (r_chdir2 < 0) ? errno : 0;
    logstep(512, "S7b-chroot+chdir2+errno", r_chroot, r_chdir2, e_chroot ? e_chroot : e_chdir2);

    if (r_move != 0 || r_chroot != 0) {
        misclog(896, "BOOT-ABORTED-MOVE-OR-CHROOT-FAILED");
        idle_forever();
    }

    pid_t pid = getpid();
    logstep(576, "S8-postpivot-getpid", (long)pid, 0, 0);

    misclog(896, "BOOT-PIVOT-OK-EXECING-REAL-INIT");

    /* Hand off to the real system. argv[0]="init" is required for
     * busybox's own multi-call dispatch to select the init applet. */
    char *argv[] = {"init", 0};
    char *envp[] = {"PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin", 0};
    execve("/bin/busybox", argv, envp);

    /* Only reached if execve itself failed. */
    int e_exec = errno;
    logstep(960, "S9-EXECVE-FAILED-errno", e_exec, 0, 0);
    idle_forever();
}
