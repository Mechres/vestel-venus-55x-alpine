/* Minimal WCNSS/Pronto power-up trigger, modeled directly on GM8's
 * gm8_wcnss_trigger.c (from the GM8 port).
 *
 * Why this exists instead of running the stock Android wcnss_service:
 * /system/bin/wcnss_service on this device is a dynamically linked Bionic
 * binary (interpreter /system/bin/linker) that also needs libcutils,
 * libutils, liblog, libqmiservices, libqmi, libqcci_legacy,
 * libqmi_client_qmux and libstdc++ -- none of which exist in an Alpine/musl
 * userspace, and the QMI libraries talk to Android's modem IPC stack, which
 * has no equivalent here. Porting that whole chain is out of scope. GM8 hit
 * the identical problem on its own WCNSS chip family and solved it the same
 * way this file does: opening /dev/wcnss_wlan and holding it open is enough
 * to power up the WCNSS subsystem, without needing the rest of the Android
 * userspace wcnss_service would otherwise require.
 *
 * The path is passed as argv[1] rather than hardcoded, because this device
 * has no devtmpfs/mdev (see /init and the vestel-wifi-hw OpenRC service):
 * /dev/wcnss_wlan's major:minor is dynamically assigned per boot, so the
 * calling service script must read /sys/class/misc/wcnss_wlan/dev, mknod
 * the node itself, and hand this program the resulting path.
 */
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	const char *path = (argc > 1) ? argv[1] : "/dev/wcnss_wlan";
	int fd;

	for (;;) {
		fd = open(path, O_RDWR);
		if (fd >= 0)
			break;
		sleep(1);
	}

	for (;;)
		pause();
}
