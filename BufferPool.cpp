#include "BufferPool.h"

BufferPool::BufferPool(size_t capacity, SpatialIndex& index) : max_capacity(capacity), spatial_index(index){
    cache.reserve(capacity);

}
void BufferPool::insert(const SpatialRecord& record){
        std::lock_guard<std::mutex>lock(pool_mutex);

        cache.push_back(record);

        //If the pool is full , trigger the flush  to the Spatial Index
        if(cache.size() >= max_capacity){
            flush_to_index();
        }
       
       
        
}
void BufferPool::flush_to_index() {
    // In Layer 2, this triggers the Z-order Morton code calculation 
    // and inserts the batch into the primary spatial tree.
        for(const auto& record : cache){
            spatial_index.insert(record);//Calculates the Z-order key automatically.
        }
        //Clear the cache to accept the new mmWAve Radar data.
    cache.clear();
} 
