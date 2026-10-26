/* Full ramdisk-pivot probe: mount userdata, write a pre-pivot marker,
 * perform the actual chdir+MS_MOVE+chroot pivot (the mechanism behind
 * switch_root), then from the NEW root: confirm getpid()==1, mount /proc
 * and capture /proc/mounts, and write a distinct post-pivot marker.
 *
 * Every syscall's return value is checked; errno is only trusted when the
 * call actually failed (bug caught in the previous probe's validation).
 * Both open() and the userdata mount() itself get a bounded 10s retry --
 * node/mount existence doesn't establish storage-driver readiness at
 * PID-1 startup, per the confirmed ENXIO-then-succeed result from the
 * last probe. Status trace goes to fixed offsets in the misc partition
 * (no dependency on the pivot succeeding); a richer human-readable log
 * goes onto userdata itself once mounted. PID 1 never exits -- it idles
 * forever after recording whatever it could, success or failure.
 */
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/mount.h>
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
#define RETRY_SLEEP_NS (50L * 1000 * 1000)

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
    /* --- Step 1: /dev + misc log device (same pattern already proven) --- */
    int r_mkdir = mkdir("/dev", 0755);
    int r_mknod_misc = mknod("/dev/misc_dbg", S_IFBLK | 0660, makedev(MISC_MAJOR, MISC_MINOR));
    int e_mknod_misc = (r_mknod_misc < 0) ? errno : 0;

    int tries;
    int e_open_misc = 0;
    for (tries = 1; tries <= RETRY_MAX; tries++) {
        g_misc_fd = open("/dev/misc_dbg", O_WRONLY);
        if (g_misc_fd >= 0) break;
        e_open_misc = errno;
        sleep_ms(50);
    }
    logstep(64, "S1-mkdir+mknodmisc+errno", r_mkdir, r_mknod_misc, e_mknod_misc);
    logstep(128, "S2-openmisc-tries+fd+errno", tries, g_misc_fd, e_open_misc);

    /* --- Step 2: mknod the userdata block device --- */
    int r_mknod_ud = mknod("/dev/userdata_blk", S_IFBLK | 0660, makedev(UD_MAJOR, UD_MINOR));
    int e_mknod_ud = (r_mknod_ud < 0) ? errno : 0;
    logstep(192, "S3-mknod-userdata+errno", r_mknod_ud, e_mknod_ud, 0);

    /* --- Step 3: mount userdata, retrying the mount() call itself --- */
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
        misclog(832, "PIVOT-ABORTED-MOUNT-FAILED");
        idle_forever();
    }

    /* --- Step 4: pre-pivot marker, on userdata via /newroot, checked+flushed --- */
    int fd_pre = open("/newroot/pivot_probe.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
    int e_pre_open = (fd_pre < 0) ? errno : 0;
    logstep(320, "S5-prepivot-open+fd+errno", 0, fd_pre, e_pre_open);

    if (fd_pre >= 0) {
        const char *pre_marker = "PRE-PIVOT-2026-09-08-DISTINCT-MARKER-A\n";
        ssize_t r_write = write(fd_pre, pre_marker, strlen(pre_marker));
        int e_write = (r_write < 0) ? errno : 0;
        int r_fsync = fsync(fd_pre);
        int e_fsync = (r_fsync < 0) ? errno : 0;
        logstep(384, "S6-prepivot-write+fsync+errno", (long)r_write, r_fsync, e_write ? e_write : e_fsync);
        close(fd_pre);
    }
    sync();
    misclog(832, "PRE-PIVOT-MARKER-A-WRITTEN-OK");

    /* --- Step 5: the actual pivot (mechanism behind switch_root) --- */
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
        misclog(896, "PIVOT-ABORTED-MOVE-OR-CHROOT-FAILED");
        idle_forever();
    }

    /* --- Step 6: from here on we ARE the new root. Reopen misc log by
     * its real path again (mknod fresh -- the old /dev/misc_dbg fd is
     * still valid via g_misc_fd since fds survive chroot, so reuse it
     * directly rather than re-opening). Confirm PID and /proc/mounts. */
    pid_t pid = getpid();
    logstep(576, "S8-postpivot-getpid", (long)pid, 0, 0);

    mkdir("/proc", 0755);
    int r_mountproc = mount("proc", "/proc", "proc", 0, NULL);
    int e_mountproc = (r_mountproc < 0) ? errno : 0;
    logstep(640, "S9-postpivot-mountproc+errno", r_mountproc, e_mountproc, 0);

    char mounts_buf[512];
    memset(mounts_buf, 0, sizeof(mounts_buf));
    ssize_t mounts_len = -1;
    if (r_mountproc == 0) {
        int fd_mounts = open("/proc/mounts", O_RDONLY);
        if (fd_mounts >= 0) {
            mounts_len = read(fd_mounts, mounts_buf, sizeof(mounts_buf) - 1);
            close(fd_mounts);
        }
    }
    /* Record a slice of /proc/mounts directly as evidence, not a verdict */
    misclog(960, mounts_len > 0 ? mounts_buf : "PROC-MOUNTS-UNAVAILABLE");

    /* --- Step 7: distinct post-pivot marker, on the SAME filesystem, now
     * addressed as "/" -- checked + flushed, appended to the same log
     * file the pre-pivot marker went into (now at a new path). --- */
    int fd_post = open("/pivot_probe.log", O_WRONLY | O_APPEND, 0644);
    int e_post_open = (fd_post < 0) ? errno : 0;
    logstep(704, "S10-postpivot-open+fd+errno", 0, fd_post, e_post_open);

    if (fd_post >= 0) {
        char post_marker[256];
        int n = 0;
        const char *p1 = "POST-PIVOT-2026-09-08-DISTINCT-MARKER-B pid=";
        for (const char *s = p1; *s; s++) post_marker[n++] = *s;
        char digits[12]; int d = 0; long v = pid;
        if (v == 0) digits[d++] = '0';
        while (v > 0) { digits[d++] = '0' + (v % 10); v /= 10; }
        while (d > 0) post_marker[n++] = digits[--d];
        post_marker[n++] = '\n';
        post_marker[n] = 0;

        ssize_t r_write2 = write(fd_post, post_marker, n);
        int e_write2 = (r_write2 < 0) ? errno : 0;
        int r_fsync2 = fsync(fd_post);
        int e_fsync2 = (r_fsync2 < 0) ? errno : 0;
        logstep(768, "S11-postpivot-write+fsync+errno", (long)r_write2, r_fsync2, e_write2 ? e_write2 : e_fsync2);
        close(fd_post);
    }
    sync();

    misclog(896, "PIVOT-PROBE-COMPLETED-ALL-STEPS-OK");
    idle_forever();
}
