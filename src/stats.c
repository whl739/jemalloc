#define JEMALLOC_STATS_C_
#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

const char *global_mutex_names[num_global_prof_mutexes] = {
#define OP(mtx) #mtx,
	GLOBAL_PROF_MUTEXES
#undef OP
};

const char *arena_mutex_names[num_arena_prof_mutexes] = {
#define OP(mtx) #mtx,
	ARENA_PROF_MUTEXES
#undef OP
};

#define CTL_GET(n, v, t) do {						\
	size_t sz = sizeof(t);						\
	xmallctl(n, (void *)v, &sz, NULL, 0);				\
} while (0)

#define CTL_M2_GET(n, i, v, t) do {					\
	size_t mib[CTL_MAX_DEPTH];					\
	size_t miblen = sizeof(mib) / sizeof(size_t);			\
	size_t sz = sizeof(t);						\
	xmallctlnametomib(n, mib, &miblen);				\
	mib[2] = (i);							\
	xmallctlbymib(mib, miblen, (void *)v, &sz, NULL, 0);		\
} while (0)

#define CTL_M2_M4_GET(n, i, j, v, t) do {				\
	size_t mib[CTL_MAX_DEPTH];					\
	size_t miblen = sizeof(mib) / sizeof(size_t);			\
	size_t sz = sizeof(t);						\
	xmallctlnametomib(n, mib, &miblen);				\
	mib[2] = (i);							\
	mib[4] = (j);							\
	xmallctlbymib(mib, miblen, (void *)v, &sz, NULL, 0);		\
} while (0)

/******************************************************************************/
/* Data. */

bool	opt_stats_print = false;

/******************************************************************************/

/* Calculate x.yyy and output a string (takes a fixed sized char array). */
static bool
get_rate_str(uint64_t dividend, uint64_t divisor, char str[6]) {
	if (divisor == 0 || dividend > divisor) {
		/* The rate is not supposed to be greater than 1. */
		return true;
	}
	if (dividend > 0) {
		assert(UINT64_MAX / dividend >= 1000);
	}

	unsigned n = (unsigned)((dividend * 1000) / divisor);
	if (n < 10) {
		malloc_snprintf(str, 6, "0.00%u", n);
	} else if (n < 100) {
		malloc_snprintf(str, 6, "0.0%u", n);
	} else if (n < 1000) {
		malloc_snprintf(str, 6, "0.%u", n);
	} else {
		malloc_snprintf(str, 6, "1");
	}

	return false;
}

#define MUTEX_CTL_STR_MAX_LENGTH 128
static void
gen_mutex_ctl_str(char *str, size_t buf_len, const char *prefix,
    const char *mutex, const char *counter) {
	malloc_snprintf(str, buf_len, "stats.%s.%s.%s", prefix, mutex, counter);
}

static void
read_arena_bin_mutex_stats(unsigned arena_ind, unsigned bin_ind,
    uint64_t results[num_mutex_prof_counters]) {
	char cmd[MUTEX_CTL_STR_MAX_LENGTH];
#define OP(c, t)							\
    gen_mutex_ctl_str(cmd, MUTEX_CTL_STR_MAX_LENGTH,			\
        "arenas.0.bins.0","mutex", #c);					\
    CTL_M2_M4_GET(cmd, arena_ind, bin_ind,				\
        (t *)&results[mutex_counter_##c], t);
MUTEX_PROF_COUNTERS
#undef OP
}

static void
mutex_stats_output_json(void (*write_cb)(void *, const char *), void *cbopaque,
    const char *name, uint64_t stats[num_mutex_prof_counters],
    const char *json_indent, bool last) {
	malloc_cprintf(write_cb, cbopaque, "%s\"%s\": {\n", json_indent, name);

	mutex_prof_counter_ind_t k = 0;
	char *fmt_str[2] = {"%s\t\"%s\": %"FMTu32"%s\n",
	    "%s\t\"%s\": %"FMTu64"%s\n"};
#define OP(c, t)							\
	malloc_cprintf(write_cb, cbopaque,				\
	    fmt_str[sizeof(t) / sizeof(uint32_t) - 1], 			\
	    json_indent, #c, (t)stats[mutex_counter_##c],		\
	    (++k == num_mutex_prof_counters) ? "" : ",");
MUTEX_PROF_COUNTERS
#undef OP
	malloc_cprintf(write_cb, cbopaque, "%s}%s\n", json_indent,
	    last ? "" : ",");
}

static void
stats_arena_bins_print(void (*write_cb)(void *, const char *), void *cbopaque,
    bool json, bool large, bool mutex, unsigned i) {
	size_t page;
	bool in_gap, in_gap_prev;
	unsigned nbins, j;

	CTL_GET("arenas.page", &page, size_t);

	CTL_GET("arenas.nbins", &nbins, unsigned);
	if (json) {
		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t\t\"bins\": [\n");
	} else {
		if (config_tcache) {
			malloc_cprintf(write_cb, cbopaque,
			    "bins:           size ind    allocated      nmalloc"
			    "      ndalloc    nrequests      curregs"
			    "     curslabs regs pgs  util       nfills"
			    "     nflushes     newslabs      reslabs"
			    "   contention  max_wait_ns\n");
		} else {
			malloc_cprintf(write_cb, cbopaque,
			    "bins:           size ind    allocated      nmalloc"
			    "      ndalloc    nrequests      curregs"
			    "     curslabs regs pgs  util     newslabs"
			    "      reslabs   contention  max_wait_ns\n");
		}
	}
	for (j = 0, in_gap = false; j < nbins; j++) {
		uint64_t nslabs;
		size_t reg_size, slab_size, curregs;
		size_t curslabs;
		uint32_t nregs;
		uint64_t nmalloc, ndalloc, nrequests, nfills, nflushes;
		uint64_t nreslabs;

		CTL_M2_M4_GET("stats.arenas.0.bins.0.nslabs", i, j, &nslabs,
		    uint64_t);
		in_gap_prev = in_gap;
		in_gap = (nslabs == 0);

		if (!json && in_gap_prev && !in_gap) {
			malloc_cprintf(write_cb, cbopaque,
			    "                     ---\n");
		}

		CTL_M2_GET("arenas.bin.0.size", j, &reg_size, size_t);
		CTL_M2_GET("arenas.bin.0.nregs", j, &nregs, uint32_t);
		CTL_M2_GET("arenas.bin.0.slab_size", j, &slab_size, size_t);

		CTL_M2_M4_GET("stats.arenas.0.bins.0.nmalloc", i, j, &nmalloc,
		    uint64_t);
		CTL_M2_M4_GET("stats.arenas.0.bins.0.ndalloc", i, j, &ndalloc,
		    uint64_t);
		CTL_M2_M4_GET("stats.arenas.0.bins.0.curregs", i, j, &curregs,
		    size_t);
		CTL_M2_M4_GET("stats.arenas.0.bins.0.nrequests", i, j,
		    &nrequests, uint64_t);
		if (config_tcache) {
			CTL_M2_M4_GET("stats.arenas.0.bins.0.nfills", i, j,
			    &nfills, uint64_t);
			CTL_M2_M4_GET("stats.arenas.0.bins.0.nflushes", i, j,
			    &nflushes, uint64_t);
		}
		CTL_M2_M4_GET("stats.arenas.0.bins.0.nreslabs", i, j, &nreslabs,
		    uint64_t);
		CTL_M2_M4_GET("stats.arenas.0.bins.0.curslabs", i, j, &curslabs,
		    size_t);

		if (json) {
			malloc_cprintf(write_cb, cbopaque,
			    "\t\t\t\t\t{\n"
			    "\t\t\t\t\t\t\"nmalloc\": %"FMTu64",\n"
			    "\t\t\t\t\t\t\"ndalloc\": %"FMTu64",\n"
			    "\t\t\t\t\t\t\"curregs\": %zu,\n"
			    "\t\t\t\t\t\t\"nrequests\": %"FMTu64",\n",
			    nmalloc,
			    ndalloc,
			    curregs,
			    nrequests);
			if (config_tcache) {
				malloc_cprintf(write_cb, cbopaque,
				    "\t\t\t\t\t\t\"nfills\": %"FMTu64",\n"
				    "\t\t\t\t\t\t\"nflushes\": %"FMTu64",\n",
				    nfills,
				    nflushes);
			}
			malloc_cprintf(write_cb, cbopaque,
			    "\t\t\t\t\t\t\"nreslabs\": %"FMTu64",\n"
			    "\t\t\t\t\t\t\"curslabs\": %zu%s\n",
			    nreslabs, curslabs, mutex ? "," : "");

			if (mutex) {
				uint64_t mutex_stats[num_mutex_prof_counters];
				read_arena_bin_mutex_stats(i, j, mutex_stats);
				mutex_stats_output_json(write_cb, cbopaque,
				    "mutex", mutex_stats, "\t\t\t\t\t\t", true);
			}
			malloc_cprintf(write_cb, cbopaque,
			    "\t\t\t\t\t}%s\n",
			    (j + 1 < nbins) ? "," : "");
		} else if (!in_gap) {
			size_t availregs = nregs * curslabs;
			char util[6];
			if (get_rate_str((uint64_t)curregs, (uint64_t)availregs,
			    util)) {
				if (availregs == 0) {
					malloc_snprintf(util, sizeof(util),
					    "1");
				} else if (curregs > availregs) {
					/*
					 * Race detected: the counters were read
					 * in separate mallctl calls and
					 * concurrent operations happened in
					 * between. In this case no meaningful
					 * utilization can be computed.
					 */
					malloc_snprintf(util, sizeof(util),
					    " race");
				} else {
					not_reached();
				}
			}
			/* Output less info for bin mutexes to save space. */
			uint64_t num_ops, num_wait, max_wait;
			CTL_M2_M4_GET("stats.arenas.0.bins.0.mutex.num_wait",
			    i, j, &num_wait, uint64_t);
			CTL_M2_M4_GET(
			    "stats.arenas.0.bins.0.mutex.max_wait_time", i, j,
			    &max_wait, uint64_t);
			CTL_M2_M4_GET("stats.arenas.0.bins.0.mutex.num_ops",
			    i, j, &num_ops, uint64_t);

			char rate[6];
			if (get_rate_str(num_wait, num_ops, rate)) {
				if (num_ops == 0) {
					malloc_snprintf(rate, sizeof(rate),
					    "0");
				}
			}

			if (config_tcache) {
				malloc_cprintf(write_cb, cbopaque,
				    "%20zu %3u %12zu %12"FMTu64
				    " %12"FMTu64" %12"FMTu64" %12zu"
				    " %12zu %4u %3zu %-5s %12"FMTu64
				    " %12"FMTu64" %12"FMTu64" %12"FMTu64
				    " %12s %12"FMTu64"\n",
				    reg_size, j, curregs * reg_size, nmalloc,
				    ndalloc, nrequests, curregs, curslabs,
				    nregs, slab_size / page, util, nfills,
				    nflushes, nslabs, nreslabs, rate, max_wait);
			} else {
				malloc_cprintf(write_cb, cbopaque,
				    "%20zu %3u %12zu %12"FMTu64
				    " %12"FMTu64" %12"FMTu64" %12zu"
				    " %12zu %4u %3zu %-5s %12"FMTu64
				    " %12"FMTu64" %12s %12"FMTu64"\n",
				    reg_size, j, curregs * reg_size, nmalloc,
				    ndalloc, nrequests, curregs, curslabs,
				    nregs, slab_size / page, util, nslabs,
				    nreslabs, rate, max_wait);
			}
		}
	}
	if (json) {
		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t\t]%s\n", large ? "," : "");
	} else {
		if (in_gap) {
			malloc_cprintf(write_cb, cbopaque,
			    "                     ---\n");
		}
	}
}

static void
stats_arena_lextents_print(void (*write_cb)(void *, const char *),
    void *cbopaque, bool json, unsigned i) {
	unsigned nbins, nlextents, j;
	bool in_gap, in_gap_prev;

	CTL_GET("arenas.nbins", &nbins, unsigned);
	CTL_GET("arenas.nlextents", &nlextents, unsigned);
	if (json) {
		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t\t\"lextents\": [\n");
	} else {
		malloc_cprintf(write_cb, cbopaque,
		    "large:          size ind    allocated      nmalloc"
		    "      ndalloc    nrequests  curlextents\n");
	}
	for (j = 0, in_gap = false; j < nlextents; j++) {
		uint64_t nmalloc, ndalloc, nrequests;
		size_t lextent_size, curlextents;

		CTL_M2_M4_GET("stats.arenas.0.lextents.0.nmalloc", i, j,
		    &nmalloc, uint64_t);
		CTL_M2_M4_GET("stats.arenas.0.lextents.0.ndalloc", i, j,
		    &ndalloc, uint64_t);
		CTL_M2_M4_GET("stats.arenas.0.lextents.0.nrequests", i, j,
		    &nrequests, uint64_t);
		in_gap_prev = in_gap;
		in_gap = (nrequests == 0);

		if (!json && in_gap_prev && !in_gap) {
			malloc_cprintf(write_cb, cbopaque,
			    "                     ---\n");
		}

		CTL_M2_GET("arenas.lextent.0.size", j, &lextent_size, size_t);
		CTL_M2_M4_GET("stats.arenas.0.lextents.0.curlextents", i, j,
		    &curlextents, size_t);
		if (json) {
			malloc_cprintf(write_cb, cbopaque,
			    "\t\t\t\t\t{\n"
			    "\t\t\t\t\t\t\"curlextents\": %zu\n"
			    "\t\t\t\t\t}%s\n",
			    curlextents,
			    (j + 1 < nlextents) ? "," : "");
		} else if (!in_gap) {
			malloc_cprintf(write_cb, cbopaque,
			    "%20zu %3u %12zu %12"FMTu64" %12"FMTu64
			    " %12"FMTu64" %12zu\n",
			    lextent_size, nbins + j,
			    curlextents * lextent_size, nmalloc, ndalloc,
			    nrequests, curlextents);
		}
	}
	if (json) {
		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t\t]\n");
	} else {
		if (in_gap) {
			malloc_cprintf(write_cb, cbopaque,
			    "                     ---\n");
		}
	}
}

static void
read_arena_mutex_stats(unsigned arena_ind,
    uint64_t results[num_arena_prof_mutexes][num_mutex_prof_counters]) {
	char cmd[MUTEX_CTL_STR_MAX_LENGTH];

	arena_prof_mutex_ind_t i;
	for (i = 0; i < num_arena_prof_mutexes; i++) {
#define OP(c, t)							\
		gen_mutex_ctl_str(cmd, MUTEX_CTL_STR_MAX_LENGTH,	\
		    "arenas.0.mutexes",	arena_mutex_names[i], #c);	\
		CTL_M2_GET(cmd, arena_ind,				\
		    (t *)&results[i][mutex_counter_##c], t);
MUTEX_PROF_COUNTERS
#undef OP
	}
}

static void
mutex_stats_output(void (*write_cb)(void *, const char *), void *cbopaque,
    const char *name, uint64_t stats[num_mutex_prof_counters],
    bool first_mutex) {
	if (first_mutex) {
		/* Print title. */
		malloc_cprintf(write_cb, cbopaque,
		    "                           n_lock_ops       n_waiting"
		    "      n_spin_acq  n_owner_switch   total_wait_ns"
		    "     max_wait_ns  max_n_thds\n");
	}

	malloc_cprintf(write_cb, cbopaque, "%s", name);
	malloc_cprintf(write_cb, cbopaque, ":%*c",
	    (int)(20 - strlen(name)), ' ');

	char *fmt_str[2] = {"%12"FMTu32, "%16"FMTu64};
#define OP(c, t)							\
	malloc_cprintf(write_cb, cbopaque,				\
	    fmt_str[sizeof(t) / sizeof(uint32_t) - 1],			\
	    (t)stats[mutex_counter_##c]);
MUTEX_PROF_COUNTERS
#undef OP
	malloc_cprintf(write_cb, cbopaque, "\n");
}

static void
stats_arena_mutexes_print(void (*write_cb)(void *, const char *),
    void *cbopaque, bool json, bool json_end, unsigned arena_ind) {
	uint64_t mutex_stats[num_arena_prof_mutexes][num_mutex_prof_counters];
	read_arena_mutex_stats(arena_ind, mutex_stats);

	/* Output mutex stats. */
	if (json) {
		malloc_cprintf(write_cb, cbopaque, "\t\t\t\t\"mutexes\": {\n");
		arena_prof_mutex_ind_t i, last_mutex;
		last_mutex = num_arena_prof_mutexes - 1;
		if (!config_tcache) {
			last_mutex--;
		}
		for (i = 0; i < num_arena_prof_mutexes; i++) {
			if (!config_tcache &&
			    i == arena_prof_mutex_tcache_list) {
				continue;
			}
			mutex_stats_output_json(write_cb, cbopaque,
			    arena_mutex_names[i], mutex_stats[i],
			    "\t\t\t\t\t", (i == last_mutex));
		}
		malloc_cprintf(write_cb, cbopaque, "\t\t\t\t}%s\n",
		    json_end ? "" : ",");
	} else {
		arena_prof_mutex_ind_t i;
		for (i = 0; i < num_arena_prof_mutexes; i++) {
			if (!config_tcache &&
			    i == arena_prof_mutex_tcache_list) {
				continue;
			}
			mutex_stats_output(write_cb, cbopaque,
			    arena_mutex_names[i], mutex_stats[i], i == 0);
		}
	}
}

static void
stats_arena_print(void (*write_cb)(void *, const char *), void *cbopaque,
    bool json, unsigned i, bool bins, bool large, bool mutex) {
	unsigned nthreads;
	const char *dss;
	ssize_t dirty_decay_time, muzzy_decay_time;
	size_t page, pactive, pdirty, pmuzzy, mapped, retained;
	size_t base, internal, resident;
	uint64_t dirty_npurge, dirty_nmadvise, dirty_purged;
	uint64_t muzzy_npurge, muzzy_nmadvise, muzzy_purged;
	size_t small_allocated;
	uint64_t small_nmalloc, small_ndalloc, small_nrequests;
	size_t large_allocated;
	uint64_t large_nmalloc, large_ndalloc, large_nrequests;
	size_t tcache_bytes;

	CTL_GET("arenas.page", &page, size_t);

	CTL_M2_GET("stats.arenas.0.nthreads", i, &nthreads, unsigned);
	if (json) {
		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t\t\"nthreads\": %u,\n", nthreads);
	} else {
		malloc_cprintf(write_cb, cbopaque,
		    "assigned threads: %u\n", nthreads);
	}

	CTL_M2_GET("stats.arenas.0.dss", i, &dss, const char *);
	if (json) {
		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t\t\"dss\": \"%s\",\n", dss);
	} else {
		malloc_cprintf(write_cb, cbopaque,
		    "dss allocation precedence: %s\n", dss);
	}

	CTL_M2_GET("stats.arenas.0.dirty_decay_time", i, &dirty_decay_time,
	    ssize_t);
	CTL_M2_GET("stats.arenas.0.muzzy_decay_time", i, &muzzy_decay_time,
	    ssize_t);
	CTL_M2_GET("stats.arenas.0.pactive", i, &pactive, size_t);
	CTL_M2_GET("stats.arenas.0.pdirty", i, &pdirty, size_t);
	CTL_M2_GET("stats.arenas.0.pmuzzy", i, &pmuzzy, size_t);
	CTL_M2_GET("stats.arenas.0.dirty_npurge", i, &dirty_npurge, uint64_t);
	CTL_M2_GET("stats.arenas.0.dirty_nmadvise", i, &dirty_nmadvise,
	    uint64_t);
	CTL_M2_GET("stats.arenas.0.dirty_purged", i, &dirty_purged, uint64_t);
	CTL_M2_GET("stats.arenas.0.muzzy_npurge", i, &muzzy_npurge, uint64_t);
	CTL_M2_GET("stats.arenas.0.muzzy_nmadvise", i, &muzzy_nmadvise,
	    uint64_t);
	CTL_M2_GET("stats.arenas.0.muzzy_purged", i, &muzzy_purged, uint64_t);
	if (json) {
		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t\t\"dirty_decay_time\": %zd,\n", dirty_decay_time);
		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t\t\"muzzy_decay_time\": %zd,\n", muzzy_decay_time);
		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t\t\"pactive\": %zu,\n", pactive);
		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t\t\"pdirty\": %zu,\n", pdirty);
		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t\t\"pmuzzy\": %zu,\n", pmuzzy);
		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t\t\"dirty_npurge\": %"FMTu64",\n", dirty_npurge);
		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t\t\"dirty_nmadvise\": %"FMTu64",\n", dirty_nmadvise);
		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t\t\"dirty_purged\": %"FMTu64",\n", dirty_purged);
		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t\t\"muzzy_npurge\": %"FMTu64",\n", muzzy_npurge);
		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t\t\"muzzy_nmadvise\": %"FMTu64",\n", muzzy_nmadvise);
		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t\t\"muzzy_purged\": %"FMTu64",\n", muzzy_purged);
	} else {
		malloc_cprintf(write_cb, cbopaque,
		    "decaying:  time       npages       sweeps     madvises"
		    "       purged\n");
		if (dirty_decay_time >= 0) {
			malloc_cprintf(write_cb, cbopaque,
			    "   dirty: %5zd %12zu %12"FMTu64" %12"FMTu64" %12"
			    FMTu64"\n", dirty_decay_time, pdirty, dirty_npurge,
			    dirty_nmadvise, dirty_purged);
		} else {
			malloc_cprintf(write_cb, cbopaque,
			    "   dirty:   N/A %12zu %12"FMTu64" %12"FMTu64" %12"
			    FMTu64"\n", pdirty, dirty_npurge, dirty_nmadvise,
			    dirty_purged);
		}
		if (muzzy_decay_time >= 0) {
			malloc_cprintf(write_cb, cbopaque,
			    "   muzzy: %5zd %12zu %12"FMTu64" %12"FMTu64" %12"
			    FMTu64"\n", muzzy_decay_time, pmuzzy, muzzy_npurge,
			    muzzy_nmadvise, muzzy_purged);
		} else {
			malloc_cprintf(write_cb, cbopaque,
			    "   muzzy:   N/A %12zu %12"FMTu64" %12"FMTu64" %12"
			    FMTu64"\n", pmuzzy, muzzy_npurge, muzzy_nmadvise,
			    muzzy_purged);
		}
	}

	CTL_M2_GET("stats.arenas.0.small.allocated", i, &small_allocated,
	    size_t);
	CTL_M2_GET("stats.arenas.0.small.nmalloc", i, &small_nmalloc, uint64_t);
	CTL_M2_GET("stats.arenas.0.small.ndalloc", i, &small_ndalloc, uint64_t);
	CTL_M2_GET("stats.arenas.0.small.nrequests", i, &small_nrequests,
	    uint64_t);
	if (json) {
		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t\t\"small\": {\n");

		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t\t\t\"allocated\": %zu,\n", small_allocated);
		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t\t\t\"nmalloc\": %"FMTu64",\n", small_nmalloc);
		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t\t\t\"ndalloc\": %"FMTu64",\n", small_ndalloc);
		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t\t\t\"nrequests\": %"FMTu64"\n", small_nrequests);

		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t\t},\n");
	} else {
		malloc_cprintf(write_cb, cbopaque,
		    "                            allocated      nmalloc"
		    "      ndalloc    nrequests\n");
		malloc_cprintf(write_cb, cbopaque,
		    "small:                   %12zu %12"FMTu64" %12"FMTu64
		    " %12"FMTu64"\n",
		    small_allocated, small_nmalloc, small_ndalloc,
		    small_nrequests);
	}

	CTL_M2_GET("stats.arenas.0.large.allocated", i, &large_allocated,
	    size_t);
	CTL_M2_GET("stats.arenas.0.large.nmalloc", i, &large_nmalloc, uint64_t);
	CTL_M2_GET("stats.arenas.0.large.ndalloc", i, &large_ndalloc, uint64_t);
	CTL_M2_GET("stats.arenas.0.large.nrequests", i, &large_nrequests,
	    uint64_t);
	if (json) {
		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t\t\"large\": {\n");

		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t\t\t\"allocated\": %zu,\n", large_allocated);
		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t\t\t\"nmalloc\": %"FMTu64",\n", large_nmalloc);
		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t\t\t\"ndalloc\": %"FMTu64",\n", large_ndalloc);
		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t\t\t\"nrequests\": %"FMTu64"\n", large_nrequests);

		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t\t},\n");
	} else {
		malloc_cprintf(write_cb, cbopaque,
		    "large:                   %12zu %12"FMTu64" %12"FMTu64
		    " %12"FMTu64"\n",
		    large_allocated, large_nmalloc, large_ndalloc,
		    large_nrequests);
		malloc_cprintf(write_cb, cbopaque,
		    "total:                   %12zu %12"FMTu64" %12"FMTu64
		    " %12"FMTu64"\n",
		    small_allocated + large_allocated, small_nmalloc +
		    large_nmalloc, small_ndalloc + large_ndalloc,
		    small_nrequests + large_nrequests);
	}
	if (!json) {
		malloc_cprintf(write_cb, cbopaque,
		    "active:                  %12zu\n", pactive * page);
	}

	CTL_M2_GET("stats.arenas.0.mapped", i, &mapped, size_t);
	if (json) {
		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t\t\"mapped\": %zu,\n", mapped);
	} else {
		malloc_cprintf(write_cb, cbopaque,
		    "mapped:                  %12zu\n", mapped);
	}

	CTL_M2_GET("stats.arenas.0.retained", i, &retained, size_t);
	if (json) {
		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t\t\"retained\": %zu,\n", retained);
	} else {
		malloc_cprintf(write_cb, cbopaque,
		    "retained:                %12zu\n", retained);
	}

	CTL_M2_GET("stats.arenas.0.base", i, &base, size_t);
	if (json) {
		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t\t\"base\": %zu,\n", base);
	} else {
		malloc_cprintf(write_cb, cbopaque,
		    "base:                    %12zu\n", base);
	}

	CTL_M2_GET("stats.arenas.0.internal", i, &internal, size_t);
	if (json) {
		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t\t\"internal\": %zu,\n", internal);
	} else {
		malloc_cprintf(write_cb, cbopaque,
		    "internal:                %12zu\n", internal);
	}

	if (config_tcache) {
		CTL_M2_GET("stats.arenas.0.tcache_bytes", i, &tcache_bytes,
		    size_t);
		if (json) {
			malloc_cprintf(write_cb, cbopaque,
			    "\t\t\t\t\"tcache\": %zu,\n", tcache_bytes);
		} else {
			malloc_cprintf(write_cb, cbopaque,
			    "tcache:                  %12zu\n", tcache_bytes);
		}
	}

	CTL_M2_GET("stats.arenas.0.resident", i, &resident, size_t);
	if (json) {
		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t\t\"resident\": %zu%s\n", resident,
		    (bins || large || mutex) ? "," : "");
	} else {
		malloc_cprintf(write_cb, cbopaque,
		    "resident:                %12zu\n", resident);
	}

	if (mutex) {
		stats_arena_mutexes_print(write_cb, cbopaque, json,
		    !(bins || large), i);
	}
	if (bins) {
		stats_arena_bins_print(write_cb, cbopaque, json, large, mutex,
		    i);
	}
	if (large) {
		stats_arena_lextents_print(write_cb, cbopaque, json, i);
	}
}

static void
stats_general_print(void (*write_cb)(void *, const char *), void *cbopaque,
    bool json, bool more) {
	const char *cpv;
	bool bv;
	unsigned uv;
	uint32_t u32v;
	uint64_t u64v;
	ssize_t ssv;
	size_t sv, bsz, usz, ssz, sssz, cpsz;

	bsz = sizeof(bool);
	usz = sizeof(unsigned);
	ssz = sizeof(size_t);
	sssz = sizeof(ssize_t);
	cpsz = sizeof(const char *);

	CTL_GET("version", &cpv, const char *);
	if (json) {
		malloc_cprintf(write_cb, cbopaque,
		"\t\t\"version\": \"%s\",\n", cpv);
	} else {
		malloc_cprintf(write_cb, cbopaque, "Version: %s\n", cpv);
	}

	/* config. */
#define CONFIG_WRITE_BOOL_JSON(n, c)					\
	if (json) {							\
		CTL_GET("config."#n, &bv, bool);			\
		malloc_cprintf(write_cb, cbopaque,			\
		    "\t\t\t\""#n"\": %s%s\n", bv ? "true" : "false",	\
		    (c));						\
	}

	if (json) {
		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\"config\": {\n");
	}

	CONFIG_WRITE_BOOL_JSON(cache_oblivious, ",")

	CTL_GET("config.debug", &bv, bool);
	if (json) {
		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t\"debug\": %s,\n", bv ? "true" : "false");
	} else {
		malloc_cprintf(write_cb, cbopaque, "Assertions %s\n",
		    bv ? "enabled" : "disabled");
	}

	CONFIG_WRITE_BOOL_JSON(fill, ",")
	CONFIG_WRITE_BOOL_JSON(lazy_lock, ",")

	if (json) {
		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t\"malloc_conf\": \"%s\",\n",
		    config_malloc_conf);
	} else {
		malloc_cprintf(write_cb, cbopaque,
		    "config.malloc_conf: \"%s\"\n", config_malloc_conf);
	}

	CONFIG_WRITE_BOOL_JSON(munmap, ",")
	CONFIG_WRITE_BOOL_JSON(prof, ",")
	CONFIG_WRITE_BOOL_JSON(prof_libgcc, ",")
	CONFIG_WRITE_BOOL_JSON(prof_libunwind, ",")
	CONFIG_WRITE_BOOL_JSON(stats, ",")
	CONFIG_WRITE_BOOL_JSON(tcache, ",")
	CONFIG_WRITE_BOOL_JSON(tls, ",")
	CONFIG_WRITE_BOOL_JSON(utrace, ",")
	CONFIG_WRITE_BOOL_JSON(xmalloc, "")

	if (json) {
		malloc_cprintf(write_cb, cbopaque,
		    "\t\t},\n");
	}
#undef CONFIG_WRITE_BOOL_JSON

	/* opt. */
#define OPT_WRITE_BOOL(n, c)						\
	if (je_mallctl("opt."#n, (void *)&bv, &bsz, NULL, 0) == 0) {	\
		if (json) {						\
			malloc_cprintf(write_cb, cbopaque,		\
			    "\t\t\t\""#n"\": %s%s\n", bv ? "true" :	\
			    "false", (c));				\
		} else {						\
			malloc_cprintf(write_cb, cbopaque,		\
			    "  opt."#n": %s\n", bv ? "true" : "false");	\
		}							\
	}
#define OPT_WRITE_BOOL_MUTABLE(n, m, c) {				\
	bool bv2;							\
	if (je_mallctl("opt."#n, (void *)&bv, &bsz, NULL, 0) == 0 &&	\
	    je_mallctl(#m, (void *)&bv2, &bsz, NULL, 0) == 0) {		\
		if (json) {						\
			malloc_cprintf(write_cb, cbopaque,		\
			    "\t\t\t\""#n"\": %s%s\n", bv ? "true" :	\
			    "false", (c));				\
		} else {						\
			malloc_cprintf(write_cb, cbopaque,		\
			    "  opt."#n": %s ("#m": %s)\n", bv ? "true"	\
			    : "false", bv2 ? "true" : "false");		\
		}							\
	}								\
}
#define OPT_WRITE_UNSIGNED(n, c)					\
	if (je_mallctl("opt."#n, (void *)&uv, &usz, NULL, 0) == 0) {	\
		if (json) {						\
			malloc_cprintf(write_cb, cbopaque,		\
			    "\t\t\t\""#n"\": %u%s\n", uv, (c));		\
		} else {						\
			malloc_cprintf(write_cb, cbopaque,		\
			"  opt."#n": %u\n", uv);			\
		}							\
	}
#define OPT_WRITE_SSIZE_T(n, c)						\
	if (je_mallctl("opt."#n, (void *)&ssv, &sssz, NULL, 0) == 0) {	\
		if (json) {						\
			malloc_cprintf(write_cb, cbopaque,		\
			    "\t\t\t\""#n"\": %zd%s\n", ssv, (c));	\
		} else {						\
			malloc_cprintf(write_cb, cbopaque,		\
			    "  opt."#n": %zd\n", ssv);			\
		}							\
	}
#define OPT_WRITE_SSIZE_T_MUTABLE(n, m, c) {				\
	ssize_t ssv2;							\
	if (je_mallctl("opt."#n, (void *)&ssv, &sssz, NULL, 0) == 0 &&	\
	    je_mallctl(#m, (void *)&ssv2, &sssz, NULL, 0) == 0) {	\
		if (json) {						\
			malloc_cprintf(write_cb, cbopaque,		\
			    "\t\t\t\""#n"\": %zd%s\n", ssv, (c));	\
		} else {						\
			malloc_cprintf(write_cb, cbopaque,		\
			    "  opt."#n": %zd ("#m": %zd)\n",		\
			    ssv, ssv2);					\
		}							\
	}								\
}
#define OPT_WRITE_CHAR_P(n, c)						\
	if (je_mallctl("opt."#n, (void *)&cpv, &cpsz, NULL, 0) == 0) {	\
		if (json) {						\
			malloc_cprintf(write_cb, cbopaque,		\
			    "\t\t\t\""#n"\": \"%s\"%s\n", cpv, (c));	\
		} else {						\
			malloc_cprintf(write_cb, cbopaque,		\
			    "  opt."#n": \"%s\"\n", cpv);		\
		}							\
	}

	if (json) {
		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\"opt\": {\n");
	} else {
		malloc_cprintf(write_cb, cbopaque,
		    "Run-time option settings:\n");
	}
	OPT_WRITE_BOOL(abort, ",")
	OPT_WRITE_CHAR_P(dss, ",")
	OPT_WRITE_UNSIGNED(narenas, ",")
	OPT_WRITE_CHAR_P(percpu_arena, ",")
	OPT_WRITE_SSIZE_T_MUTABLE(dirty_decay_time, arenas.dirty_decay_time,
	    ",")
	OPT_WRITE_SSIZE_T_MUTABLE(muzzy_decay_time, arenas.muzzy_decay_time,
	    ",")
	OPT_WRITE_CHAR_P(junk, ",")
	OPT_WRITE_BOOL(zero, ",")
	OPT_WRITE_BOOL(utrace, ",")
	OPT_WRITE_BOOL(xmalloc, ",")
	OPT_WRITE_BOOL(tcache, ",")
	OPT_WRITE_SSIZE_T(lg_tcache_max, ",")
	OPT_WRITE_BOOL(prof, ",")
	OPT_WRITE_CHAR_P(prof_prefix, ",")
	OPT_WRITE_BOOL_MUTABLE(prof_active, prof.active, ",")
	OPT_WRITE_BOOL_MUTABLE(prof_thread_active_init, prof.thread_active_init,
	    ",")
	OPT_WRITE_SSIZE_T_MUTABLE(lg_prof_sample, prof.lg_sample, ",")
	OPT_WRITE_BOOL(prof_accum, ",")
	OPT_WRITE_SSIZE_T(lg_prof_interval, ",")
	OPT_WRITE_BOOL(prof_gdump, ",")
	OPT_WRITE_BOOL(prof_final, ",")
	OPT_WRITE_BOOL(prof_leak, ",")
	/*
	 * stats_print is always emitted, so as long as stats_print comes last
	 * it's safe to unconditionally omit the comma here (rather than having
	 * to conditionally omit it elsewhere depending on configuration).
	 */
	OPT_WRITE_BOOL(stats_print, "")
	if (json) {
		malloc_cprintf(write_cb, cbopaque,
		    "\t\t},\n");
	}

#undef OPT_WRITE_BOOL
#undef OPT_WRITE_BOOL_MUTABLE
#undef OPT_WRITE_SSIZE_T
#undef OPT_WRITE_CHAR_P

	/* arenas. */
	if (json) {
		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\"arenas\": {\n");
	}

	CTL_GET("arenas.narenas", &uv, unsigned);
	if (json) {
		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t\"narenas\": %u,\n", uv);
	} else {
		malloc_cprintf(write_cb, cbopaque, "Arenas: %u\n", uv);
	}

	CTL_GET("arenas.dirty_decay_time", &ssv, ssize_t);
	if (json) {
		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t\"dirty_decay_time\": %zd,\n", ssv);
	} else {
		malloc_cprintf(write_cb, cbopaque,
		    "Unused dirty page decay time: %zd%s\n", ssv, (ssv < 0) ?
		    " (no decay)" : "");
	}

	CTL_GET("arenas.muzzy_decay_time", &ssv, ssize_t);
	if (json) {
		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t\"muzzy_decay_time\": %zd,\n", ssv);
	} else {
		malloc_cprintf(write_cb, cbopaque,
		    "Unused muzzy page decay time: %zd%s\n", ssv, (ssv < 0) ?
		    " (no decay)" : "");
	}

	CTL_GET("arenas.quantum", &sv, size_t);
	if (json) {
		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t\"quantum\": %zu,\n", sv);
	} else {
		malloc_cprintf(write_cb, cbopaque, "Quantum size: %zu\n", sv);
	}

	CTL_GET("arenas.page", &sv, size_t);
	if (json) {
		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t\"page\": %zu,\n", sv);
	} else {
		malloc_cprintf(write_cb, cbopaque, "Page size: %zu\n", sv);
	}

	if (je_mallctl("arenas.tcache_max", (void *)&sv, &ssz, NULL, 0) == 0) {
		if (json) {
			malloc_cprintf(write_cb, cbopaque,
			    "\t\t\t\"tcache_max\": %zu,\n", sv);
		} else {
			malloc_cprintf(write_cb, cbopaque,
			    "Maximum thread-cached size class: %zu\n", sv);
		}
	}

	if (json) {
		unsigned nbins, nlextents, i;

		CTL_GET("arenas.nbins", &nbins, unsigned);
		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t\"nbins\": %u,\n", nbins);

		if (config_tcache) {
			CTL_GET("arenas.nhbins", &uv, unsigned);
			malloc_cprintf(write_cb, cbopaque,
			    "\t\t\t\"nhbins\": %u,\n", uv);
		}

		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t\"bin\": [\n");
		for (i = 0; i < nbins; i++) {
			malloc_cprintf(write_cb, cbopaque,
			    "\t\t\t\t{\n");

			CTL_M2_GET("arenas.bin.0.size", i, &sv, size_t);
			malloc_cprintf(write_cb, cbopaque,
			    "\t\t\t\t\t\"size\": %zu,\n", sv);

			CTL_M2_GET("arenas.bin.0.nregs", i, &u32v, uint32_t);
			malloc_cprintf(write_cb, cbopaque,
			    "\t\t\t\t\t\"nregs\": %"FMTu32",\n", u32v);

			CTL_M2_GET("arenas.bin.0.slab_size", i, &sv, size_t);
			malloc_cprintf(write_cb, cbopaque,
			    "\t\t\t\t\t\"slab_size\": %zu\n", sv);

			malloc_cprintf(write_cb, cbopaque,
			    "\t\t\t\t}%s\n", (i + 1 < nbins) ? "," : "");
		}
		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t],\n");

		CTL_GET("arenas.nlextents", &nlextents, unsigned);
		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t\"nlextents\": %u,\n", nlextents);

		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t\"lextent\": [\n");
		for (i = 0; i < nlextents; i++) {
			malloc_cprintf(write_cb, cbopaque,
			    "\t\t\t\t{\n");

			CTL_M2_GET("arenas.lextent.0.size", i, &sv, size_t);
			malloc_cprintf(write_cb, cbopaque,
			    "\t\t\t\t\t\"size\": %zu\n", sv);

			malloc_cprintf(write_cb, cbopaque,
			    "\t\t\t\t}%s\n", (i + 1 < nlextents) ? "," : "");
		}
		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t]\n");

		malloc_cprintf(write_cb, cbopaque,
		    "\t\t}%s\n", (config_prof || more) ? "," : "");
	}

	/* prof. */
	if (config_prof && json) {
		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\"prof\": {\n");

		CTL_GET("prof.thread_active_init", &bv, bool);
		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t\"thread_active_init\": %s,\n", bv ? "true" :
		    "false");

		CTL_GET("prof.active", &bv, bool);
		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t\"active\": %s,\n", bv ? "true" : "false");

		CTL_GET("prof.gdump", &bv, bool);
		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t\"gdump\": %s,\n", bv ? "true" : "false");

		CTL_GET("prof.interval", &u64v, uint64_t);
		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t\"interval\": %"FMTu64",\n", u64v);

		CTL_GET("prof.lg_sample", &ssv, ssize_t);
		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t\"lg_sample\": %zd\n", ssv);

		malloc_cprintf(write_cb, cbopaque,
		    "\t\t}%s\n", more ? "," : "");
	}
}

static void
read_global_mutex_stats(
    uint64_t results[num_global_prof_mutexes][num_mutex_prof_counters]) {
	char cmd[MUTEX_CTL_STR_MAX_LENGTH];

	global_prof_mutex_ind_t i;
	for (i = 0; i < num_global_prof_mutexes; i++) {
#define OP(c, t)							\
		gen_mutex_ctl_str(cmd, MUTEX_CTL_STR_MAX_LENGTH,	\
		    "mutexes", global_mutex_names[i], #c);		\
		CTL_GET(cmd, (t *)&results[i][mutex_counter_##c], t);
MUTEX_PROF_COUNTERS
#undef OP
	}
}

static void
stats_print_helper(void (*write_cb)(void *, const char *), void *cbopaque,
    bool json, bool merged, bool destroyed, bool unmerged, bool bins,
    bool large, bool mutex) {
	size_t allocated, active, metadata, resident, mapped, retained;

	CTL_GET("stats.allocated", &allocated, size_t);
	CTL_GET("stats.active", &active, size_t);
	CTL_GET("stats.metadata", &metadata, size_t);
	CTL_GET("stats.resident", &resident, size_t);
	CTL_GET("stats.mapped", &mapped, size_t);
	CTL_GET("stats.retained", &retained, size_t);

	uint64_t mutex_stats[num_global_prof_mutexes][num_mutex_prof_counters];
	if (mutex) {
		read_global_mutex_stats(mutex_stats);
	}

	if (json) {
		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\"stats\": {\n");

		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t\"allocated\": %zu,\n", allocated);
		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t\"active\": %zu,\n", active);
		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t\"metadata\": %zu,\n", metadata);
		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t\"resident\": %zu,\n", resident);
		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t\"mapped\": %zu,\n", mapped);
		malloc_cprintf(write_cb, cbopaque,
		    "\t\t\t\"retained\": %zu%s\n", retained, mutex ? "," : "");
		if (mutex) {
			malloc_cprintf(write_cb, cbopaque,
			    "\t\t\t\"mutexes\": {\n");
			global_prof_mutex_ind_t i;
			for (i = 0; i < num_global_prof_mutexes; i++) {
				mutex_stats_output_json(write_cb, cbopaque,
				    global_mutex_names[i], mutex_stats[i],
				    "\t\t\t\t",
				    i == num_global_prof_mutexes - 1);
			}
			malloc_cprintf(write_cb, cbopaque, "\t\t\t}\n");
		}
		malloc_cprintf(write_cb, cbopaque,
		    "\t\t}%s\n", (merged || unmerged || destroyed) ? "," : "");
	} else {
		malloc_cprintf(write_cb, cbopaque,
		    "Allocated: %zu, active: %zu, metadata: %zu,"
		    " resident: %zu, mapped: %zu, retained: %zu\n",
		    allocated, active, metadata, resident, mapped, retained);
		if (mutex) {
			global_prof_mutex_ind_t i;
			for (i = 0; i < num_global_prof_mutexes; i++) {
				mutex_stats_output(write_cb, cbopaque,
				    global_mutex_names[i], mutex_stats[i],
				    i == 0);
			}
		}
	}

	if (merged || destroyed || unmerged) {
		unsigned narenas;

		if (json) {
			malloc_cprintf(write_cb, cbopaque,
			    "\t\t\"stats.arenas\": {\n");
		}

		CTL_GET("arenas.narenas", &narenas, unsigned);
		{
			size_t mib[3];
			size_t miblen = sizeof(mib) / sizeof(size_t);
			size_t sz;
			VARIABLE_ARRAY(bool, initialized, narenas);
			bool destroyed_initialized;
			unsigned i, j, ninitialized;

			xmallctlnametomib("arena.0.initialized", mib, &miblen);
			for (i = ninitialized = 0; i < narenas; i++) {
				mib[1] = i;
				sz = sizeof(bool);
				xmallctlbymib(mib, miblen, &initialized[i], &sz,
				    NULL, 0);
				if (initialized[i]) {
					ninitialized++;
				}
			}
			mib[1] = MALLCTL_ARENAS_DESTROYED;
			sz = sizeof(bool);
			xmallctlbymib(mib, miblen, &destroyed_initialized, &sz,
			    NULL, 0);

			/* Merged stats. */
			if (merged && (ninitialized > 1 || !unmerged)) {
				/* Print merged arena stats. */
				if (json) {
					malloc_cprintf(write_cb, cbopaque,
					    "\t\t\t\"merged\": {\n");
				} else {
					malloc_cprintf(write_cb, cbopaque,
					    "\nMerged arenas stats:\n");
				}
				stats_arena_print(write_cb, cbopaque, json,
				    MALLCTL_ARENAS_ALL, bins, large, mutex);
				if (json) {
					malloc_cprintf(write_cb, cbopaque,
					    "\t\t\t}%s\n",
					    ((destroyed_initialized &&
					    destroyed) || unmerged) ?  "," :
					    "");
				}
			}

			/* Destroyed stats. */
			if (destroyed_initialized && destroyed) {
				/* Print destroyed arena stats. */
				if (json) {
					malloc_cprintf(write_cb, cbopaque,
					    "\t\t\t\"destroyed\": {\n");
				} else {
					malloc_cprintf(write_cb, cbopaque,
					    "\nDestroyed arenas stats:\n");
				}
				stats_arena_print(write_cb, cbopaque, json,
				    MALLCTL_ARENAS_DESTROYED, bins, large,
				    mutex);
				if (json) {
					malloc_cprintf(write_cb, cbopaque,
					    "\t\t\t}%s\n", unmerged ?  "," :
					    "");
				}
			}

			/* Unmerged stats. */
			if (unmerged) {
				for (i = j = 0; i < narenas; i++) {
					if (initialized[i]) {
						if (json) {
							j++;
							malloc_cprintf(write_cb,
							    cbopaque,
							    "\t\t\t\"%u\": {\n",
							    i);
						} else {
							malloc_cprintf(write_cb,
							    cbopaque,
							    "\narenas[%u]:\n",
							    i);
						}
						stats_arena_print(write_cb,
						    cbopaque, json, i, bins,
						    large, mutex);
						if (json) {
							malloc_cprintf(write_cb,
							    cbopaque,
							    "\t\t\t}%s\n", (j <
							    ninitialized) ? ","
							    : "");
						}
					}
				}
			}
		}

		if (json) {
			malloc_cprintf(write_cb, cbopaque,
			    "\t\t}\n");
		}
	}
}

void
stats_print(void (*write_cb)(void *, const char *), void *cbopaque,
    const char *opts) {
	int err;
	uint64_t epoch;
	size_t u64sz;
	bool json = false;
	bool general = true;
	bool merged = config_stats;
	bool destroyed = config_stats;
	bool unmerged = config_stats;
	bool bins = true;
	bool large = true;
	bool mutex = true;

	/*
	 * Refresh stats, in case mallctl() was called by the application.
	 *
	 * Check for OOM here, since refreshing the ctl cache can trigger
	 * allocation.  In practice, none of the subsequent mallctl()-related
	 * calls in this function will cause OOM if this one succeeds.
	 * */
	epoch = 1;
	u64sz = sizeof(uint64_t);
	err = je_mallctl("epoch", (void *)&epoch, &u64sz, (void *)&epoch,
	    sizeof(uint64_t));
	if (err != 0) {
		if (err == EAGAIN) {
			malloc_write("<jemalloc>: Memory allocation failure in "
			    "mallctl(\"epoch\", ...)\n");
			return;
		}
		malloc_write("<jemalloc>: Failure in mallctl(\"epoch\", "
		    "...)\n");
		abort();
	}

	if (opts != NULL) {
		unsigned i;

		for (i = 0; opts[i] != '\0'; i++) {
			switch (opts[i]) {
			case 'J':
				json = true;
				break;
			case 'g':
				general = false;
				break;
			case 'm':
				merged = false;
				break;
			case 'd':
				destroyed = false;
				break;
			case 'a':
				unmerged = false;
				break;
			case 'b':
				bins = false;
				break;
			case 'l':
				large = false;
				break;
			case 'x':
				mutex = false;
				break;
			default:;
			}
		}
	}

	if (json) {
		malloc_cprintf(write_cb, cbopaque,
		    "{\n"
		    "\t\"jemalloc\": {\n");
	} else {
		malloc_cprintf(write_cb, cbopaque,
		    "___ Begin jemalloc statistics ___\n");
	}

	if (general) {
		stats_general_print(write_cb, cbopaque, json, config_stats);
	}
	if (config_stats) {
		stats_print_helper(write_cb, cbopaque, json, merged, destroyed,
		    unmerged, bins, large, mutex);
	}

	if (json) {
		malloc_cprintf(write_cb, cbopaque,
		    "\t}\n"
		    "}\n");
	} else {
		malloc_cprintf(write_cb, cbopaque,
		    "--- End jemalloc statistics ---\n");
	}
}
