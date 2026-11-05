// libstdc++'s nothrow new calls plain new inside a try: a failed allocation goes through
// __cxa_allocate_exception, and with CONFIG_COMPILER_CXX_EXCEPTIONS_EMG_POOL_SIZE=0 that
// terminates when the heap cannot give it about 112 bytes either. At that point
// makeUniqueNoThrow, the code built to survive this, aborts instead of returning null.
// Replacing the pair (the standard allows it) goes straight to malloc, costs no DRAM, and
// needs no sdkconfig change, which would rebuild the framework.

#ifndef SIMULATOR

#include <cstdlib>
#include <new>

void* operator new(const std::size_t size, const std::nothrow_t&) noexcept { return malloc(size ? size : 1); }
void* operator new[](const std::size_t size, const std::nothrow_t&) noexcept { return malloc(size ? size : 1); }

#endif  // SIMULATOR
