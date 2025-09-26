#include "memory/object_pool.hpp"
#include "structures/order_book.hpp"
#include "network/udp_receiver.hpp"

namespace hft {

// Explicit instantiations for commonly used types
template class ObjectPool<Order, 10000>;
template class ObjectPool<NetworkMessage, 100000>;

template class PODObjectPool<int, 1000>;
template class PODObjectPool<uint64_t, 1000>;
template class PODObjectPool<double, 1000>;

} // namespace hft
