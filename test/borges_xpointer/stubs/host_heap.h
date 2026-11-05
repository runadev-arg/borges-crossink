// Routes expat's allocations through the counters in xpdiff.cpp, to measure peak heap on the host.
#pragma once
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
void* hmalloc(size_t);
void* hrealloc(void*, size_t);
void hfree(void*);
#ifdef __cplusplus
}
#endif
