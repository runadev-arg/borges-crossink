// mingw has no aligned_alloc; the host only needs the allocation.
#pragma once
#include <stdlib.h>
inline void* aligned_alloc(size_t, size_t size) { return malloc(size); }
#include <cstdlib>
