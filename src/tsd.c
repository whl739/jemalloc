#define JEMALLOC_TSD_C_
#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

/******************************************************************************/
/* Data. */

static unsigned ncleanups;
static malloc_tsd_cleanup_t cleanups[MALLOC_TSD_CLEANUPS_MAX];

malloc_tsd_data(, , tsd_t, TSD_INITIALIZER)

/******************************************************************************/

void
tsd_slow_update(tsd_t *tsd) {
	if (tsd_nominal(tsd)) {
		if (malloc_slow || !tsd->tcache_enabled ||
		    tsd_reentrancy_level_get(tsd) > 0) {
			tsd->state = tsd_state_nominal_slow;
		} else {
			tsd->state = tsd_state_nominal;
		}
	}
}

tsd_t *
tsd_fetch_slow(tsd_t *tsd) {
	if (tsd->state == tsd_state_nominal_slow) {
		/* On slow path but no work needed. */
		assert(malloc_slow || !tsd_tcache_enabled_get(tsd) ||
		    tsd_reentrancy_level_get(tsd) > 0 ||
		    *tsd_arenas_tdata_bypassp_get(tsd));
	} else if (tsd->state == tsd_state_uninitialized) {
		tsd->state = tsd_state_nominal;
		tsd_slow_update(tsd);
		/* Trigger cleanup handler registration. */
		tsd_set(tsd);
		tsd_data_init(tsd);
	} else if (tsd->state == tsd_state_purgatory) {
		tsd->state = tsd_state_reincarnated;
		tsd_set(tsd);
		tsd_data_init(tsd);
	} else {
		assert(tsd->state == tsd_state_reincarnated);
	}

	return tsd;
}

void *
malloc_tsd_malloc(size_t size) {
	return a0malloc(CACHELINE_CEILING(size));
}

void
malloc_tsd_dalloc(void *wrapper) {
	a0dalloc(wrapper);
}

#if defined(JEMALLOC_MALLOC_THREAD_CLEANUP) || defined(_WIN32)
#ifndef _WIN32
JEMALLOC_EXPORT
#endif
void
_malloc_thread_cleanup(void) {
	bool pending[MALLOC_TSD_CLEANUPS_MAX], again;
	unsigned i;

	for (i = 0; i < ncleanups; i++) {
		pending[i] = true;
	}

	do {
		again = false;
		for (i = 0; i < ncleanups; i++) {
			if (pending[i]) {
				pending[i] = cleanups[i]();
				if (pending[i]) {
					again = true;
				}
			}
		}
	} while (again);
}
#endif

void
malloc_tsd_cleanup_register(bool (*f)(void)) {
	assert(ncleanups < MALLOC_TSD_CLEANUPS_MAX);
	cleanups[ncleanups] = f;
	ncleanups++;
}

bool
tsd_data_init(void *arg) {
	tsd_t *tsd = (tsd_t *)arg;
#define MALLOC_TSD_init_yes(n, t)					\
	if (tsd_##n##_data_init(tsd)) {					\
		return true;						\
	}
#define MALLOC_TSD_init_no(n, t)
#define O(n, t, gs, i, c)						\
	MALLOC_TSD_init_##i(n, t)
MALLOC_TSD
#undef MALLOC_TSD_init_yes
#undef MALLOC_TSD_init_no
#undef O
	return false;
}

void
tsd_cleanup(void *arg) {
	tsd_t *tsd = (tsd_t *)arg;

	switch (tsd->state) {
	case tsd_state_uninitialized:
		/* Do nothing. */
		break;
	case tsd_state_nominal:
	case tsd_state_nominal_slow:
	case tsd_state_reincarnated:
		/*
		 * Reincarnated means another destructor deallocated memory
		 * after this destructor was called.  Reset state to
		 * tsd_state_purgatory and request another callback.
		 */
#define MALLOC_TSD_cleanup_yes(n, t)					\
		n##_cleanup(tsd);
#define MALLOC_TSD_cleanup_no(n, t)
#define O(n, t, gs, i, c)						\
		MALLOC_TSD_cleanup_##c(n, t)
MALLOC_TSD
#undef MALLOC_TSD_cleanup_yes
#undef MALLOC_TSD_cleanup_no
#undef O
		tsd->state = tsd_state_purgatory;
		tsd_set(tsd);
		break;
	case tsd_state_purgatory:
		/*
		 * The previous time this destructor was called, we set the
		 * state to tsd_state_purgatory so that other destructors
		 * wouldn't cause re-creation of the tsd.  This time, do
		 * nothing, and do not request another callback.
		 */
		break;
	default:
		not_reached();
	}
}

tsd_t *
malloc_tsd_boot0(void) {
	tsd_t *tsd;

	ncleanups = 0;
	if (tsd_boot0()) {
		return NULL;
	}
	tsd = tsd_fetch();
	*tsd_arenas_tdata_bypassp_get(tsd) = true;
	return tsd;
}

void
malloc_tsd_boot1(void) {
	tsd_boot1();
	tsd_t *tsd = tsd_fetch();
	/* malloc_slow has been set properly.  Update tsd_slow. */
	tsd_slow_update(tsd);
	*tsd_arenas_tdata_bypassp_get(tsd) = false;
}

#ifdef _WIN32
static BOOL WINAPI
_tls_callback(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
	switch (fdwReason) {
#ifdef JEMALLOC_LAZY_LOCK
	case DLL_THREAD_ATTACH:
		isthreaded = true;
		break;
#endif
	case DLL_THREAD_DETACH:
		_malloc_thread_cleanup();
		break;
	default:
		break;
	}
	return true;
}

/*
 * We need to be able to say "read" here (in the "pragma section"), but have
 * hooked "read". We won't read for the rest of the file, so we can get away
 * with unhooking.
 */
#ifdef read
#  undef read
#endif

#ifdef _MSC_VER
#  ifdef _M_IX86
#    pragma comment(linker, "/INCLUDE:__tls_used")
#    pragma comment(linker, "/INCLUDE:_tls_callback")
#  else
#    pragma comment(linker, "/INCLUDE:_tls_used")
#    pragma comment(linker, "/INCLUDE:tls_callback")
#  endif
#  pragma section(".CRT$XLY",long,read)
#endif
JEMALLOC_SECTION(".CRT$XLY") JEMALLOC_ATTR(used)
BOOL	(WINAPI *const tls_callback)(HINSTANCE hinstDLL,
    DWORD fdwReason, LPVOID lpvReserved) = _tls_callback;
#endif

#if (!defined(JEMALLOC_MALLOC_THREAD_CLEANUP) && !defined(JEMALLOC_TLS) && \
    !defined(_WIN32))
void *
tsd_init_check_recursion(tsd_init_head_t *head, tsd_init_block_t *block) {
	pthread_t self = pthread_self();
	tsd_init_block_t *iter;

	/* Check whether this thread has already inserted into the list. */
	malloc_mutex_lock(TSDN_NULL, &head->lock);
	ql_foreach(iter, &head->blocks, link) {
		if (iter->thread == self) {
			malloc_mutex_unlock(TSDN_NULL, &head->lock);
			return iter->data;
		}
	}
	/* Insert block into list. */
	ql_elm_new(block, link);
	block->thread = self;
	ql_tail_insert(&head->blocks, block, link);
	malloc_mutex_unlock(TSDN_NULL, &head->lock);
	return NULL;
}

void
tsd_init_finish(tsd_init_head_t *head, tsd_init_block_t *block) {
	malloc_mutex_lock(TSDN_NULL, &head->lock);
	ql_remove(&head->blocks, block, link);
	malloc_mutex_unlock(TSDN_NULL, &head->lock);
}
#endif
