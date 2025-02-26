/*
 * Copyright (c) 2013 Hugh Bailey <obs.jim@gmail.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <stdlib.h>
#include <string.h>
#include "base.h"
#include "bmem.h"
#include "platform.h"
#include "threading.h"

/*
 * NOTE: totally jacked the mem alignment trick from ffmpeg, credit to them:
 *   http://www.ffmpeg.org/
 */

//PRISM/WangShaohui/20210429/NoIssue/monitor memory overflow
//#define ALIGNMENT 32

/* TODO: use memalign for non-windows systems */
#if defined(_WIN32)
#define ALIGNED_MALLOC 1
#else
#define ALIGNMENT_HACK 1
#endif

static void *a_malloc(size_t size)
{
#ifdef ALIGNED_MALLOC
	return _aligned_malloc(size, ALIGNMENT);
#elif ALIGNMENT_HACK
	void *ptr = NULL;
	long diff;

	ptr = malloc(size + ALIGNMENT);
	if (ptr) {
		diff = ((~(long)ptr) & (ALIGNMENT - 1)) + 1;
		ptr = (char *)ptr + diff;
		((char *)ptr)[-1] = (char)diff;
	}

	return ptr;
#else
	return malloc(size);
#endif
}

static void *a_realloc(void *ptr, size_t size)
{
#ifdef ALIGNED_MALLOC
	return _aligned_realloc(ptr, size, ALIGNMENT);
#elif ALIGNMENT_HACK
	long diff;

	if (!ptr)
		return a_malloc(size);
	diff = ((char *)ptr)[-1];
	ptr = realloc((char *)ptr - diff, size + diff);
	if (ptr)
		ptr = (char *)ptr + diff;
	return ptr;
#else
	return realloc(ptr, size);
#endif
}

static void a_free(void *ptr)
{
#ifdef ALIGNED_MALLOC
	_aligned_free(ptr);
#elif ALIGNMENT_HACK
	if (ptr)
		free((char *)ptr - ((char *)ptr)[-1]);
#else
	free(ptr);
#endif
}

struct base_allocator alloc = {a_malloc, a_realloc, a_free};
long num_allocs = 0;

//PRISM/WangShaohui/20210301/NoIssue/debug breakpoint
static unsigned failed_memory_length = 0;
unsigned mem_failed_length()
{
	return failed_memory_length;
}

void base_set_allocator(struct base_allocator *defs)
{
	memcpy(&alloc, defs, sizeof(struct base_allocator));
}

//PRISM/WangShaohui/20210301/NoIssue/fix breakpoint
#define MALLOC_RETRY_TIMES 5
void *bmalloc_inner(size_t size, bool break_if_fail)
{
	void *ptr = alloc.malloc(size);
	if (!ptr && !size)
		ptr = alloc.malloc(1);

	//PRISM/WangShaohui/20210301/NoIssue/fix breakpoint
	if (!ptr) {
		for (int i = 0; i < MALLOC_RETRY_TIMES; i++) {
			ptr = alloc.malloc(size);
			if (ptr) {
				plog(LOG_INFO,
				     "Successed to bmalloc memory for %llu bytes at the %dth time",
				     size, i + 1);
				break;
			}
		}
		if (!ptr) {
			plog(LOG_WARNING,
			     "Failed to request memory with %llu bytes", size);
		}
	}

	//PRISM/WangShaohui/20210301/NoIssue/fix breakpoint
	if (!ptr && break_if_fail) {
		//PRISM/WangShaohui/20210301/NoIssue/debug breakpoint
		failed_memory_length = (unsigned)size;

		os_breakpoint();
		bcrash("Out of memory while trying to allocate %lu bytes",
		       (unsigned long)size);
	}

	os_atomic_inc_long(&num_allocs);
	return ptr;
}

//PRISM/WangShaohui/20210301/NoIssue/fix breakpoint
void *brealloc_inner(void *ptr, size_t size, bool break_if_fail)
{
	if (!ptr)
		os_atomic_inc_long(&num_allocs);

	ptr = alloc.realloc(ptr, size);
	if (!ptr && !size)
		ptr = alloc.realloc(ptr, 1);

	//PRISM/WangShaohui/20210301/NoIssue/fix breakpoint
	if (!ptr) {
		for (int i = 0; i < MALLOC_RETRY_TIMES; i++) {
			ptr = alloc.realloc(ptr, size);
			if (ptr) {
				plog(LOG_INFO,
				     "Successed to brealloc memory for %llu bytes at the %dth time",
				     size, i + 1);
				break;
			}
		}
		if (!ptr) {
			plog(LOG_WARNING,
			     "Failed to request memory with %llu bytes", size);
		}
	}

	//PRISM/WangShaohui/20210301/NoIssue/fix breakpoint
	if (!ptr && break_if_fail) {
		//PRISM/WangShaohui/20210301/NoIssue/debug breakpoint
		failed_memory_length = (unsigned)size;

		os_breakpoint();
		bcrash("Out of memory while trying to allocate %lu bytes",
		       (unsigned long)size);
	}

	return ptr;
}

//PRISM/WangShaohui/20210301/NoIssue/fix breakpoint
void *pls_bmalloc(size_t size, const char *func)
{
	void *ret = bmalloc_inner(size, false);
	if (!ret) {
		plog(LOG_ERROR,
		     "(pls_bmalloc) Failed to request memory with %lluBytes in %s",
		     size, func ? func : "unknownFunc");
	}
	return ret;
}

//PRISM/WangShaohui/20210301/NoIssue/fix breakpoint
void *pls_brealloc(void *ptr, size_t size, const char *func)
{
	void *ret = brealloc_inner(ptr, size, false);
	if (!ret) {
		plog(LOG_ERROR,
		     "(pls_brealloc) Failed to request memory with %lluBytes in %s",
		     size, func ? func : "unknownFunc");
	}
	return ret;
}

void *bmalloc(size_t size)
{
	//PRISM/WangShaohui/20210301/NoIssue/fix breakpoint
	return bmalloc_inner(size, true);
}

void *brealloc(void *ptr, size_t size)
{
	//PRISM/WangShaohui/20210301/NoIssue/fix breakpoint
	return brealloc_inner(ptr, size, true);
}

void bfree(void *ptr)
{
	if (ptr)
		os_atomic_dec_long(&num_allocs);
	alloc.free(ptr);
}

long bnum_allocs(void)
{
	return num_allocs;
}

int base_get_alignment(void)
{
	return ALIGNMENT;
}

void *bmemdup(const void *ptr, size_t size)
{
	void *out = bmalloc(size);
	if (size)
		memcpy(out, ptr, size);

	return out;
}
