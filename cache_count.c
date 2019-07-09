#define _GNU_SOURCE

#include <sys/types.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <locale.h>
#include <sys/prctl.h>
#include <err.h>

#include <perfmon/pfmlib_perf_event.h>
#include "perf_util.h"

#include <sys/mman.h>
#include <x86intrin.h>
#include <sched.h>
#include <sys/sysinfo.h>

void *
mmap_size(size_t size) {
    return mmap(NULL, size, PROT_READ | PROT_WRITE | PROT_EXEC,
            MAP_ANONYMOUS | MAP_PRIVATE | MAP_POPULATE,
            -1, 0);
}
uint64_t current_cpu;

void set_affinity(uint32_t cpuid) {
    cpu_set_t cpu_set;
    CPU_ZERO(&cpu_set);
    CPU_SET(cpuid, &cpu_set);
    sched_setaffinity(0, sizeof(cpu_set_t), &cpu_set);
    current_cpu = cpuid;
}
static const char *gen_events[]={
	"instructions",
    "branch-instructions",
    "branch-misses",
	"cache-misses",
    "inst_retired.any_p",
    "frontend_retired.dsb_miss",
    "idq.all_dsb_cycles_any_uops",
    "idq.ms_uops",
    "idq.dsb_uops",
    "uops_executed.core",
    "uops_dispatched_port.port_0",
    "uops_dispatched_port.port_1",
    "uops_dispatched_port.port_2",
    "uops_dispatched_port.port_3",
    "uops_dispatched_port.port_4",
    "uops_dispatched_port.port_5",
    "uops_dispatched_port.port_6",
    "uops_dispatched_port.port_7",
    "uops_issued.any",
    "br_misp_retired.all_branches",
	NULL
};

uint64_t init_counts[100];
uint64_t final_counts[100];
uint64_t real_counts[100];

static void
measure_counts(perf_event_desc_t *fds, int num_fds, uint64_t *counters)
{
	uint64_t values[3];
    uint64_t val;
    uint64_t ratio;
    int ret;
	for (int i = 0; i < num_fds; i++) {
		ret = read(fds[i].fd, values, sizeof(values));
		if (ret < (ssize_t)sizeof(values)) {
			if (ret == -1)
				err(1, "cannot read results: %s", strerror(errno));
			else
				warnx("could not read event%d", i);
		}
		/*
		 * scaling is systematic because we may be sharing the PMU and
		 * thus may be multiplexed
		 */
		val = perf_scale(values);
		ratio = perf_scale_ratio(values);
        counters[i] = val;
	}
}

volatile uint64_t integer_value = 0;

char *scratchpad;

void
warmup() {
    for (int i=0; i<100000000; ++i) {
        integer_value += 1;
    }
    printf("Integer_value %ld\n", integer_value);
}
volatile uint64_t branch_value = 0;
uint32_t tsc;

void
do_something() {
    integer_value = 0;
    uint32_t a;
    uint32_t status;
    for (int i=0; i<1000000; ++i) {
        scratchpad[0] = scratchpad[1];
        _mm_clflush((void*)&scratchpad);
    }
    __rdtscp(&tsc);
    _mm_mfence();
}
/*
90                              NOP1_OVERRIDE_NOP
6690                            NOP2_OVERRIDE_NOP
0f1f00                          NOP3_OVERRIDE_NOP
0f1f4000                        NOP4_OVERRIDE_NOP
0f1f440000                      NOP5_OVERRIDE_NOP
660f1f440000                    NOP6_OVERRIDE_NOP
0f1f8000000000                  NOP7_OVERRIDE_NOP
0f1f840000000000                NOP8_OVERRIDE_NOP
660f1f840000000000              NOP9_OVERRIDE_NOP
66660f1f840000000000            NOP10_OVERRIDE_NOP
6666660f1f840000000000          NOP11_OVERRIDE_NOP
*/
char instruction_seq[] = "\x66\x90\xc3";
//char instruction_seq[] = "\x66\x66\x66\x0f\x1f\x84\x00\x00\x00\x00\x00\xc3";
size_t ins_seq_len = 0;

int
main(int argc, char **argv)
{
	perf_event_desc_t *fds = NULL;
	int i, ret, num_fds = 0;

    printf("# of CPUs available %d\n", get_nprocs());
    set_affinity(get_nprocs()-1);
    printf("My affinity %d\n", sched_getcpu());

    scratchpad = mmap_size(0x1000);
    printf("MMAP AT: %p\n", scratchpad);

    memset(scratchpad, 0xc3, 0x1000);
    memcpy(scratchpad, instruction_seq, ins_seq_len);

    //((int(*)())scratchpad)();

	setlocale(LC_ALL, "");
	/*
	 * Initialize pfm library (required before we can use it)
	 */

	ret = pfm_initialize();

	if (ret != PFM_SUCCESS)
		errx(1, "Cannot initialize library: %s", pfm_strerror(ret));

	ret = perf_setup_argv_events(argc > 1 ? (const char **)argv+1 : gen_events, &fds, &num_fds);
	if (ret || !num_fds)
		errx(1, "cannot setup events");

	fds[0].fd = -1;
	for(i=0; i < num_fds; i++) {
        fds[i].cpu = current_cpu;

		/* request timing information necessary for scaling */
		fds[i].hw.read_format = PERF_FORMAT_SCALE;

		fds[i].hw.disabled = 1; /* do not start now */
        fds[i].hw.exclude_kernel = 1;
        fds[i].hw.exclude_hv = 1;
        fds[i].hw.exclude_idle = 1;

		/* each event is in an independent group (multiplexing likely) */
		fds[i].fd = perf_event_open(&fds[i].hw, 0, fds[i].cpu, -1, 0);
		if (fds[i].fd == -1)
			err(1, "cannot open event %d", i);
	}

    warmup();
	/*
	 * enable all counters attached to this thread and created by it
	 */
	ret = prctl(PR_TASK_PERF_EVENTS_ENABLE);
	if (ret)
		err(1, "prctl(enable) failed");

    _mm_mfence();
    __rdtscp(&tsc);

    // before doing something, measure the counters.
    measure_counts(fds, num_fds, init_counts);

    // do something
    do_something();


	/*
	 * disable all counters attached to this thread
	 */
	ret = prctl(PR_TASK_PERF_EVENTS_DISABLE);
	if (ret)
		err(1, "prctl(disable) failed");

    _mm_mfence();
    __rdtscp(&tsc);


    measure_counts(fds, num_fds, final_counts);


    for (int i=0; i<num_fds; ++i) {
        real_counts[i] = final_counts[i] - init_counts[i];
        printf("%-30s:\t%ld\n", fds[i].name, real_counts[i]);
    }


    // close all opened fds

	for (i = 0; i < num_fds; i++)
	  close(fds[i].fd);

	perf_free_fds(fds, num_fds);

	/* free libpfm resources cleanly */
	pfm_terminate();

	return 0;
}
