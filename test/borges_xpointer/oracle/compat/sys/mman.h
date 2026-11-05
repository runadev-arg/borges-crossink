// Host shim: crengine only maps files for its on-disk cache, which the oracle disables.
#pragma once
#include <stddef.h>
#define PROT_READ 1
#define PROT_WRITE 2
#define MAP_SHARED 1
#define MAP_FAILED ((void*)-1)
static inline void* mmap(void*, size_t, int, int, int, long) { return MAP_FAILED; }
static inline int munmap(void*, size_t) { return 0; }
static inline int msync(void*, size_t, int) { return 0; }
#define MS_SYNC 0
