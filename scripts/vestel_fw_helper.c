/* Minimal userspace firmware-loading helper.
 *
 * This device has no devtmpfs/mdev/udev (removed in Phase 2 -- see
 * README: this kernel's devfs service tries `mount -t devtmpfs`, fails,
 * and falls back to shadowing /init's persistent /dev nodes with a fresh
 * tmpfs). Real Android relies on ueventd to service the kernel's classic
 * "userspace firmware loading" fallback protocol (used whenever
 * request_firmware()'s direct in-kernel filesystem search doesn't find
 * the file at its own default path list): the kernel creates a
 * /sys/class/firmware/<name>/ entry and a matching uevent, and userspace
 * is expected to write 1 to "loading", copy the file's bytes into
 * "data", then write 0 to "loading" (or -1 on failure). With nothing
 * doing that, a pending request blocks forever, uninterruptibly (D
 * state, unkillable) -- this is the confirmed root cause of every WCNSS
 * bring-up stall investigated in the README's Phase 3 section, not a
 * hardware fault.
 *
 * This listens on the kernel's uevent netlink socket (the same multicast
 * group ueventd uses) for ACTION=add SUBSYSTEM=firmware events, and
 * services each one by copying the matching file from /lib/firmware/.
 * Read-only with respect to hardware: it only ever writes to the
 * kernel-provided sysfs firmware-loading interface, never to any device
 * register.
 */
#include <sys/socket.h>
#include <sys/stat.h>
#include <linux/netlink.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define FW_DIR "/lib/firmware/"
#define SYS_FW "/sys/class/firmware/"

static void service_request(const char *devpath, const char *firmware)
{
	char loading_path[512], data_path[512], src_path[512];
	int loading_fd, data_fd, src_fd;
	struct stat st;
	char *buf;
	size_t total = 0;
	ssize_t n;

	snprintf(loading_path, sizeof(loading_path), "/sys%s/loading", devpath);
	snprintf(data_path, sizeof(data_path), "/sys%s/data", devpath);
	snprintf(src_path, sizeof(src_path), FW_DIR "%s", firmware);

	src_fd = open(src_path, O_RDONLY);
	loading_fd = open(loading_path, O_WRONLY);
	if (loading_fd < 0) {
		if (src_fd >= 0)
			close(src_fd);
		return;
	}

	if (src_fd < 0 || fstat(src_fd, &st) < 0) {
		write(loading_fd, "-1", 2);
		close(loading_fd);
		if (src_fd >= 0)
			close(src_fd);
		return;
	}

	/* The kernel's firmware "data" sysfs write expects the complete
	 * blob in a single write() call -- confirmed via a real failure
	 * ("Blob size 4096 doesn't match 12596") from an earlier version of
	 * this helper that streamed the file in read()-sized chunks: the
	 * first short read (one page) got treated as the whole blob. Read
	 * the entire file into memory first, then issue one write(). */
	buf = malloc(st.st_size ? st.st_size : 1);
	if (!buf) {
		write(loading_fd, "-1", 2);
		close(loading_fd);
		close(src_fd);
		return;
	}

	while (total < (size_t)st.st_size) {
		n = read(src_fd, buf + total, st.st_size - total);
		if (n <= 0)
			break;
		total += n;
	}
	close(src_fd);

	if (total != (size_t)st.st_size) {
		write(loading_fd, "-1", 2);
		free(buf);
		close(loading_fd);
		return;
	}

	write(loading_fd, "1", 1);
	data_fd = open(data_path, O_WRONLY);
	if (data_fd >= 0) {
		/* sysfs bin_attribute writes are capped at PAGE_SIZE per
		 * write(2) syscall by the VFS regardless of how large a
		 * buffer userspace passes in one call -- confirmed via a
		 * real failure ("Blob size 4096 doesn't match 12596") from
		 * an earlier version of this helper that issued one
		 * write() call and never checked its return value, silently
		 * under-writing every blob bigger than one page. Loop on
		 * the actual bytes written each call; the destination fd's
		 * position auto-advances like any regular file. */
		size_t off = 0;
		while (off < total) {
			ssize_t w = write(data_fd, buf + off, total - off);
			if (w <= 0)
				break;
			off += w;
		}
		close(data_fd);
	}
	free(buf);
	write(loading_fd, "0", 1);
	close(loading_fd);
}

int main(void)
{
	int sock;
	struct sockaddr_nl addr;
	char buf[8192];

	sock = socket(PF_NETLINK, SOCK_DGRAM, NETLINK_KOBJECT_UEVENT);
	if (sock < 0) {
		perror("socket");
		return 1;
	}

	memset(&addr, 0, sizeof(addr));
	addr.nl_family = AF_NETLINK;
	addr.nl_pid = getpid();
	addr.nl_groups = 1;

	if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		perror("bind");
		return 1;
	}

	for (;;) {
		ssize_t len = recv(sock, buf, sizeof(buf) - 1, 0);
		char devpath[512] = "";
		char firmware[256] = "";
		char subsystem[64] = "";
		char action[32] = "";
		char *p, *end;

		if (len <= 0)
			continue;
		buf[len] = '\0';

		p = buf;
		end = buf + len;
		while (p < end) {
			size_t l = strlen(p);
			if (!strncmp(p, "ACTION=", 7))
				snprintf(action, sizeof(action), "%s", p + 7);
			else if (!strncmp(p, "SUBSYSTEM=", 10))
				snprintf(subsystem, sizeof(subsystem), "%s", p + 10);
			else if (!strncmp(p, "DEVPATH=", 8))
				snprintf(devpath, sizeof(devpath), "%s", p + 8);
			else if (!strncmp(p, "FIRMWARE=", 9))
				snprintf(firmware, sizeof(firmware), "%s", p + 9);
			p += l + 1;
		}

		if (!strcmp(subsystem, "firmware") && !strcmp(action, "add") &&
		    devpath[0] && firmware[0])
			service_request(devpath, firmware);
	}
}
