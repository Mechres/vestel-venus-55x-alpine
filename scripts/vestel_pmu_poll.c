/* Passive, read-only PMU register poller for the Pronto/WCNSS block.
 *
 * Maps the pmu_base MMIO window (fb21b000-fb21dfff, confirmed via this
 * device's /proc/iomem) via /dev/mem and repeatedly reads the specific
 * registers arch/arm/mach-msm/pil-pronto.c (the public Nexus 5 "flo"
 * 3.4-kitkat reference driver for the same Pronto IP block; Vestel's own
 * driver source is not available, but this basic v1 sequence -- clear the
 * GDSC power-collapse bit, enable AHB/CPU clocks, poll for clock-ready,
 * deassert CCPU reset -- is expected to be structurally similar for this
 * chip family) writes/polls during subsystem bring-up:
 *
 *   0x24   PRONTO_PMU_COMMON_GDSCR        bit0 = SW_COLLAPSE
 *   0x30   PRONTO_PMU_COMMON_CPU_CBCR     bit0 = CLK_EN, bit31 = CLK_OFF
 *   0x34   PRONTO_PMU_COMMON_AHB_CBCR     bit0 = CLK_EN, bit31 = CLK_OFF
 *   0x1040 PRONTO_PMU_COMMON_CSR          bit0 = A2XB_CFG_EN
 *   0x104C PRONTO_PMU_SOFT_RESET          bit10 = CRCM_CCPU_SOFT_RESET
 *   0x2000 PRONTO_PMU_CCPU_CTL            bit0 = HIGH_IVT, bit2 = REMAP_EN
 *   0x2004 PRONTO_PMU_CCPU_BOOT_REMAP_ADDR
 *
 * Purpose: this device's own hang investigation (see README Phase 3
 * section) found that opening /dev/wcnss_wlan with both required firmware
 * files present hangs the kernel completely with no further log output,
 * unlike two earlier graceful failures (missing firmware) whose captured
 * PMU dumps showed none of this reset sequence had even started. This
 * tool polls the same registers from a *separate* process while the
 * trigger runs, to see exactly which register write (if any) the real
 * hardware sequence gets stuck on before the whole system stops
 * responding. Read-only: never writes to any register.
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define PMU_PHYS_BASE 0xfb21b000UL
#define PMU_MAP_SIZE  0x3000UL

struct reg_def {
	const char *name;
	uint32_t offset;
};

static const struct reg_def regs[] = {
	{ "GDSCR", 0x24 },
	{ "CPU_CBCR", 0x30 },
	{ "AHB_CBCR", 0x34 },
	{ "COMMON_CSR", 0x1040 },
	{ "SOFT_RESET", 0x104C },
	{ "CCPU_CTL", 0x2000 },
	{ "CCPU_BOOT_REMAP", 0x2004 },
};

static void ts_now(char *buf, size_t n)
{
	struct timespec ts;
	struct tm tmv;
	clock_gettime(CLOCK_REALTIME, &ts);
	localtime_r(&ts.tv_sec, &tmv);
	snprintf(buf, n, "%02d:%02d:%02d.%03ld",
		 tmv.tm_hour, tmv.tm_min, tmv.tm_sec, ts.tv_nsec / 1000000);
}

int main(int argc, char **argv)
{
	int fd;
	volatile uint8_t *map;
	char tbuf[32];
	int iters = (argc > 1) ? atoi(argv[1]) : 200;
	int delay_ms = (argc > 2) ? atoi(argv[2]) : 50;

	fd = open("/dev/mem", O_RDONLY | O_SYNC);
	if (fd < 0) {
		perror("open /dev/mem");
		return 1;
	}

	map = mmap(NULL, PMU_MAP_SIZE, PROT_READ, MAP_SHARED, fd,
		   PMU_PHYS_BASE);
	if (map == MAP_FAILED) {
		perror("mmap");
		return 1;
	}

	setvbuf(stdout, NULL, _IOLBF, 0);

	for (int i = 0; i < iters; i++) {
		ts_now(tbuf, sizeof(tbuf));
		printf("%s", tbuf);
		for (size_t r = 0; r < sizeof(regs) / sizeof(regs[0]); r++) {
			uint32_t v = *(volatile uint32_t *)(map + regs[r].offset);
			printf(" %s=%08x", regs[r].name, v);
		}
		printf("\n");
		usleep(delay_ms * 1000);
	}

	munmap((void *)map, PMU_MAP_SIZE);
	close(fd);
	return 0;
}
