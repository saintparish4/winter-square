#include "structures/lock_free_queue.hpp"

namespace hft {

// Template instantiations for common types
template class LockFreeQueue<void*, 65536>;
template class LockFreeQueue<void*, 32768>;
template class LockFreeQueue<void*, 16384>;

template class SPSCLockFreeQueue<int, 1024>;
template class SPSCLockFreeQueue<uint64_t, 4096>;

} // namespace hft
