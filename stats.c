/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */

/*
 * Copyright 2009 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

#include <stdio.h>
#include <fcntl.h>
#include <sys/types.h>
#include <stdarg.h>
#include <limits.h>

#include "filebench.h"
#include "flowop.h"
#include "vars.h"
#include "stats.h"
#include "fbtime.h"

/*
 * A set of routines for collecting and dumping various filebench
 * run statistics.
 */

/* Global statistics */
static struct flowstats *globalstats = NULL;
static hrtime_t stats_cputime = 0;

#ifdef HAVE_LIBKSTAT
#include <kstat.h>
#include <sys/cpuvar.h>

static kstat_ctl_t *kstatp = NULL;
static kstat_t *sysinfo_ksp = NULL;
static kstat_t **cpu_kstat_list = NULL;
static int kstat_ncpus = 0;

static int
stats_build_kstat_list(void)
{
	kstat_t *ksp;

	kstat_ncpus = 0;
	for (ksp = kstatp->kc_chain; ksp; ksp = ksp->ks_next)
		if (strncmp(ksp->ks_name, "cpu_stat", 8) == 0)
			kstat_ncpus++;

	if ((cpu_kstat_list = (kstat_t **)
	    malloc(kstat_ncpus * sizeof (kstat_t *))) == NULL) {
		filebench_log(LOG_ERROR, "malloc failed");
		return (FILEBENCH_ERROR);
	}

	kstat_ncpus = 0;
	for (ksp = kstatp->kc_chain; ksp; ksp = ksp->ks_next)
		if (strncmp(ksp->ks_name, "cpu_stat", 8) == 0 &&
		    kstat_read(kstatp, ksp, NULL) != -1)
			cpu_kstat_list[kstat_ncpus++] = ksp;

	if (kstat_ncpus == 0) {
		filebench_log(LOG_ERROR,
		    "kstats can't find any cpu statistics");
		return (FILEBENCH_ERROR);
	}

	return (FILEBENCH_OK);
}

static int
stats_kstat_update(void)
{
	if (kstatp == NULL) {
		if ((kstatp = kstat_open()) == (kstat_ctl_t *)NULL) {
			filebench_log(LOG_ERROR, "Cannot read kstats");
			return (FILEBENCH_ERROR);
		}
	}

	/* get the sysinfo kstat */
	if (sysinfo_ksp == NULL)
		sysinfo_ksp = kstat_lookup(kstatp, "unix", 0, "sysinfo");

	/* get per cpu kstats, if necessary */
	if (cpu_kstat_list == NULL) {

		/* Initialize the array of cpu kstat pointers */
		if (stats_build_kstat_list() == FILEBENCH_ERROR)
			return (FILEBENCH_ERROR);

	} else if (kstat_chain_update(kstatp) != 0) {

		/* free up current array of kstat ptrs and get new one */
		free((void *)cpu_kstat_list);
		if (stats_build_kstat_list() == FILEBENCH_ERROR)
			return (FILEBENCH_ERROR);
	}

	return (FILEBENCH_OK);
}

/*
 * Uses the kstat library or, if it is not available, the /proc/stat file
 * to obtain cpu statistics. Collects statistics for each cpu, initializes
 * a local pointer to the sysinfo kstat, and returns the sum of user and
 * kernel time for all the cpus.
 */
static hrtime_t
kstats_read_cpu(void)
{
	u_longlong_t	cputime_states[CPU_STATES];
	hrtime_t	cputime;
	int		i;

	/*
	 * Per-CPU statistics
	 */

	if (stats_kstat_update() == FILEBENCH_ERROR)
		return (0);

	/* Sum across all CPUs */
	(void) memset(&cputime_states, 0, sizeof (cputime_states));
	for (i = 0; i < kstat_ncpus; i++) {
		cpu_stat_t cpu_stats;
		int j;

		(void) kstat_read(kstatp, cpu_kstat_list[i],
		    (void *) &cpu_stats);
		for (j = 0; j < CPU_STATES; j++)
			cputime_states[j] += cpu_stats.cpu_sysinfo.cpu[j];
	}

	cputime = cputime_states[CPU_KERNEL] + cputime_states[CPU_USER];

	return (10000000LL * cputime);
}
#elif defined(HAVE_PROC_STAT)
static hrtime_t
kstats_read_cpu(void)
{
	/*
	 * Linux provides system wide statistics in /proc/stat
	 * The entry for cpu is
	 * cpu  1636 67 1392 208671 5407 20 12
	 * cpu0 626 8 997 104476 2499 7 7
	 * cpu1 1010 58 395 104195 2907 13 5
	 *
	 * The number of jiffies (1/100ths of  a  second)  that  the
	 * system  spent  in  user mode, user mode with low priority
	 * (nice), system mode, and  the  idle  task,  respectively.
	 */
	FILE *statfd;
	hrtime_t user, nice, system;
	char cpu[128]; /* placeholder to read "cpu" */

	statfd = fopen("/proc/stat", "r");
	if (!statfd) {
		filebench_log(LOG_ERROR, "Cannot open /proc/stat");
		return (-1);
	}
#if defined(_LP64) || (__WORDSIZE == 64)
	if (fscanf(statfd, "%s %lu %lu %lu", cpu, &user, &nice, &system) != 4) {
#else
	if (fscanf(statfd, "%s %llu %llu %llu", cpu, &user, &nice, &system) != 4) {
#endif
		filebench_log(LOG_ERROR, "Cannot read /proc/stat");
		fclose(statfd);
		return (-1);
	}
	

	fclose(statfd);
	
	/* convert jiffies to nanosecs */
	return ((user+nice+system)*10000000);
}
#else
static hrtime_t
kstats_read_cpu(void)
{
	filebench_log(LOG_ERROR, "No /proc/stat or libkstat,"
	 	"so no correct source of per-system CPU usage!");
	return (-1);
}
#endif

/*
 * Returns the net cpu time used since the beginning of the run.
 * Just calls kstat_read_cpu() and subtracts stats_cputime which
 * is set at the beginning of the filebench run.
 */
static hrtime_t
kstats_read_cpu_relative(void)
{
	hrtime_t cputime;

	cputime = kstats_read_cpu();
	return (cputime - stats_cputime);
}

/*
 * Initializes the static variable "stats_cputime" with the
 * current cpu time, for use by kstats_read_cpu_relative.
 */
void
stats_init(void)
{
	stats_cputime = kstats_read_cpu();
}

/*
 * Add a flowstat b to a, leave sum in a.
 */
static void
stats_add(struct flowstats *a, struct flowstats *b)
{
	int i;

	a->fs_count += b->fs_count;
	a->fs_rcount += b->fs_rcount;
	a->fs_wcount += b->fs_wcount;
	a->fs_bytes += b->fs_bytes;
	a->fs_rbytes += b->fs_rbytes;
	a->fs_wbytes += b->fs_wbytes;
	a->fs_total_lat += b->fs_total_lat;

	if (b->fs_maxlat > a->fs_maxlat)
		a->fs_maxlat = b->fs_maxlat;

	if (b->fs_minlat < a->fs_minlat)
		a->fs_minlat = b->fs_minlat;

	for (i = 0; i < OSPROF_BUCKET_NUMBER; i++)
		a->fs_distribution[i] += b->fs_distribution[i];
}

/*
 * Takes a "snapshot" of the global statistics. Actually, it calculates
 * them from the local statistics maintained by each flowop.
 * First the routine pauses filebench, then rolls the statistics for
 * each flowop into its associated FLOW_MASTER flowop.
 * Next all the FLOW_MASTER flowops' statistics are written
 * to the log file followed by the global totals. Then filebench
 * operation is allowed to resume.
 */
void
stats_snap(void)
{
	struct flowstats *iostat = &globalstats[FLOW_TYPE_IO];
	struct flowstats *aiostat = &globalstats[FLOW_TYPE_AIO];
	hrtime_t cputime;
	hrtime_t orig_starttime;
	flowop_t *flowop;
	char *str;
	double total_time_sec;

	if (!globalstats) {
		filebench_log(LOG_ERROR,
		    "'stats snap' called before 'stats clear'");
		return;
	}

	/* don't print out if run ended in error */
	if (filebench_shm->shm_f_abort == FILEBENCH_ABORT_ERROR) {
		filebench_log(LOG_ERROR,
		    "NO VALID RESULTS! Filebench run terminated prematurely");
		return;
	}

	/* Freeze statistics during update */
	filebench_shm->shm_bequiet = 1;

	/* We want to have blank global statistics each
	 * time we start the summation process, but the
	 * statistics collection start time must remain
	 * unchanged (it's a snapshot compared to the original
	 * start time). */
	orig_starttime = globalstats->fs_stime;
	(void) memset(globalstats, 0, FLOW_TYPES * sizeof(struct flowstats));
	globalstats->fs_stime = orig_starttime;
	globalstats->fs_etime = gethrtime();

	total_time_sec = (globalstats->fs_etime -
			globalstats->fs_stime) / SEC2NS_FLOAT;
	filebench_log(LOG_DEBUG_SCRIPT, "Stats period = %.0f sec",
			total_time_sec);

	/* Similarly we blank the master flowop statistics */
	flowop = filebench_shm->shm_flowoplist;
	while (flowop) {
		if (flowop->fo_instance == FLOW_MASTER) {
			(void) memset(&flowop->fo_stats, 0, sizeof(struct flowstats));
			flowop->fo_stats.fs_minlat = ULLONG_MAX;
		}
		flowop = flowop->fo_next;
	}

	/* Roll up per-flowop statistics in globalstats and master flowops */
	flowop = filebench_shm->shm_flowoplist;
	while (flowop) {
		flowop_t *flowop_master;

		if (flowop->fo_instance <= FLOW_DEFINITION) {
			flowop = flowop->fo_next;
			continue;
		}

		/* Roll up per-flowop into global stats */
		stats_add(&globalstats[flowop->fo_type], &flowop->fo_stats);
		stats_add(&globalstats[FLOW_TYPE_GLOBAL], &flowop->fo_stats);

		flowop_master = flowop_find_one(flowop->fo_name, FLOW_MASTER);
		if (flowop_master) {
			/* Roll up per-flowop stats into master */
			stats_add(&flowop_master->fo_stats, &flowop->fo_stats);
		} else {
			filebench_log(LOG_DEBUG_NEVER,
			    "flowop_stats could not find %s",
			    flowop->fo_name);
		}

		filebench_log(LOG_DEBUG_SCRIPT,
		    "flowop %-20s-%4d  - %5d ops %5.1lf ops/sec %5.1lfmb/s "
		    "%8.3fms/op",
		    flowop->fo_name,
		    flowop->fo_instance,
		    flowop->fo_stats.fs_count,
		    flowop->fo_stats.fs_count / total_time_sec,
		    (flowop->fo_stats.fs_bytes / MB_FLOAT) / total_time_sec,
		    flowop->fo_stats.fs_count ?
		    flowop->fo_stats.fs_total_lat /
		    (flowop->fo_stats.fs_count * SEC2MS_FLOAT) : 0);

		flowop = flowop->fo_next;

	}

	cputime = kstats_read_cpu_relative();

	flowop = filebench_shm->shm_flowoplist;
	str = malloc(1048576);
	*str = '\0';
	(void) strcpy(str, "Per-Operation Breakdown\n");
	while (flowop) {
		char line[1024];
		char histogram[1024];
		char hist_reading[20];
		int i = 0;

		if (flowop->fo_instance != FLOW_MASTER) {
			flowop = flowop->fo_next;
			continue;
		}

		(void) snprintf(line, sizeof(line), "%-20s %dops %8.0lfops/s "
		    "%5.1lfmb/s %8.1fms/op",
		    flowop->fo_name,
		    flowop->fo_stats.fs_count,
		    flowop->fo_stats.fs_count / total_time_sec,
		    (flowop->fo_stats.fs_bytes / MB_FLOAT) / total_time_sec,
		    flowop->fo_stats.fs_count ?
		    flowop->fo_stats.fs_total_lat /
		    (flowop->fo_stats.fs_count * SEC2MS_FLOAT) : 0);
		(void) strcat(str, line);

		(void) snprintf(line, sizeof(line)," [%.2fms - %5.2fms]",
			flowop->fo_stats.fs_minlat / SEC2MS_FLOAT,
			flowop->fo_stats.fs_maxlat / SEC2MS_FLOAT);
		(void) strcat(str, line);

		if (filebench_shm->lathist_enabled) {
			(void) sprintf(histogram, "\t[ ");
			for (i = 0; i < OSPROF_BUCKET_NUMBER; i++) {
				(void) sprintf(hist_reading, "%lu ",
				flowop->fo_stats.fs_distribution[i]);
				(void) strcat(histogram, hist_reading);
			}
			(void) strcat(histogram, "]\n");
			(void) strcat(str, histogram);
		} else
			(void) strcat(str, "\n");

		flowop = flowop->fo_next;
	}

	/* removing last \n  */
	str[strlen(str) - 1] = '\0';

	filebench_log(LOG_INFO, "%s", str);
	free(str);

	filebench_log(LOG_INFO,
	    "IO Summary: %5d ops, %5.3lf ops/s, %0.0lf/%0.0lf rd/wr, "
	    "%5.1lfmb/s, %6.0fus cpu/op, %5.1fms latency",
	    iostat->fs_count + aiostat->fs_count,
	    (iostat->fs_count + aiostat->fs_count) / total_time_sec,
	    (iostat->fs_rcount + aiostat->fs_rcount) / total_time_sec,
	    (iostat->fs_wcount + aiostat->fs_wcount) / total_time_sec,
	    ((iostat->fs_bytes + aiostat->fs_bytes) / MB_FLOAT)
						/ total_time_sec,
	    (iostat->fs_rcount + iostat->fs_wcount +
	    aiostat->fs_rcount + aiostat->fs_wcount) ?
	    (iostat->fs_syscpu / 1000.0) /
	    (iostat->fs_rcount + iostat->fs_wcount +
	    aiostat->fs_rcount + aiostat->fs_wcount) : 0,
	    (iostat->fs_rcount + iostat->fs_wcount) ?
	    iostat->fs_total_lat /
	    ((iostat->fs_rcount + iostat->fs_wcount) * SEC2MS_FLOAT) : 0);

	filebench_shm->shm_bequiet = 0;
}

/*
 * Clears all the statistics variables (fo_stats) for every defined flowop.
 * It also creates a global flowstat table if one doesn't already exist and
 * clears it.
 */
void
stats_clear(void)
{
	flowop_t *flowop;

	stats_cputime = kstats_read_cpu();

	if (globalstats == NULL)
		globalstats = malloc(FLOW_TYPES * sizeof (struct flowstats));

	(void) memset(globalstats, 0, FLOW_TYPES * sizeof (struct flowstats));

	flowop = filebench_shm->shm_flowoplist;

	while (flowop) {
		filebench_log(LOG_DEBUG_IMPL, "Clearing stats for %s-%d",
		    flowop->fo_name,
		    flowop->fo_instance);
		(void) memset(&flowop->fo_stats, 0, sizeof (struct flowstats));
		flowop = flowop->fo_next;
	}

	(void) memset(globalstats, 0, sizeof(struct flowstats));
	globalstats->fs_stime = gethrtime();
}
