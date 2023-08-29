//
// Created by jan on 25.8.2023.
//

#include <string.h>
#include <assert.h>
#include "../include/jvm.h"
#include "internal.h"

#undef jvm_buffer_create
#undef jvm_image_create

static const VkAllocationCallbacks* allocator_vk_callbacks(const jvm_allocator* this)
{
    return this->has_vk_alloc ? &this->vk_allocation_callbacks : NULL;
}

static void free_pool(jvm_allocator* this, jvm_allocation_pool* pool)
{
    for (unsigned i = 0; i < pool->chunk_count; ++i)
    {
        jvm_free(this, pool->chunks[i]);
    }
    jvm_free(this, pool->chunks);
    vkFreeMemory(this->device, pool->memory, allocator_vk_callbacks(this));
    jvm_free(this, pool);
}

void jvm_allocator_destroy(jvm_allocator* allocator)
{
    for (unsigned i = 0; i < allocator->pool_count; ++i)
    {
        jvm_allocation_pool* const pool = allocator->pools[i];
        const unsigned chunks_left = pool->chunk_count;
        if (chunks_left > 1 )
        {
            JVM_ERROR(allocator, "Pool at index %u has %u chunks left, which were not free-d yet", i, chunks_left);
        }
#ifdef JVM_TRACK_ALLOCATIONS
        for (unsigned j = 0; j < pool->chunk_count; ++j)
        {
            jvm_chunk* const chunk = pool->chunks[j];
            if (chunk->used == 0)
            {
                continue;
            }
            JVM_ERROR(allocator, "Chunk allocated at %s:%d was not free-d", chunk->file, chunk->line);
        }
#endif
        free_pool(allocator, pool);
    }
    jvm_free(allocator, allocator->pools);
    jvm_free(allocator, allocator);
}

static VkResult create_new_pool(jvm_allocator* this, VkDeviceSize mem_size, uint32_t idx, VkMemoryType mem_info)
{
    jvm_allocation_pool* const pool = jvm_alloc(this, sizeof(*pool));
    if (!pool)
    {
        JVM_ERROR(this, "Could not allocate memory for memory pool");
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    if (this->pool_capacity == this->pool_count)
    {
        const unsigned new_capacity = (this->pool_count ? this->pool_count : 8) << 1;
        jvm_allocation_pool** const new_ptr = jvm_realloc(
                this, this->pools, sizeof(jvm_allocation_pool*) * new_capacity);
        if (!new_ptr)
        {
            JVM_ERROR(this, "Could not reallocate memory for pool memory");
            jvm_free(this, pool);
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        this->pools = new_ptr;
        this->pool_capacity = new_capacity;
    }
    pool->chunk_count = 1;
    pool->chunk_capacity = 32;
    pool->chunks = jvm_alloc(this, sizeof(*pool->chunks) * pool->chunk_capacity);
    if (!pool->chunks)
    {
        JVM_ERROR(this, "Could not allocate memory for the pool chunk array");
        jvm_free(this, pool);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }

    jvm_chunk* const whole_chunk = jvm_alloc(this, sizeof(*whole_chunk));
    if (!whole_chunk)
    {
        JVM_ERROR(this, "Could not allocate memory for the pool's initial chunk");
        jvm_free(this, pool->chunks);
        jvm_free(this, pool);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }

    pool->map_count = 0;
    pool->map_ptr = NULL;

    pool->memory_type_index = idx;
    pool->memory_type_info = mem_info;
    pool->size = mem_size;

    VkMemoryAllocateInfo allocate_info =
            {
                    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                    .allocationSize = mem_size,
                    .memoryTypeIndex = idx,
            };
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkResult res = vkAllocateMemory(this->device, &allocate_info, allocator_vk_callbacks(this), &mem);
    if (res != VK_SUCCESS)
    {
        JVM_ERROR(this, "Could not allocate device memory");
        jvm_free(this, pool);
        return res;
    }
    *whole_chunk = (jvm_chunk)
            {
                    .chunk_offset = 0,
                    .size = mem_size,
                    .padding = 0,
                    .used = 0,
                    .mapped = 0,
                    .memory = mem,
                    .pool = pool,
            };
    pool->chunks[0] = whole_chunk;

    pool->memory = mem;

    this->pools[this->pool_count] = pool;
    this->pool_count += 1;
    return VK_SUCCESS;
}

//  Returns 0 on success, -1 when pool is not from this allocator, -2 when there are still chunks within the pool
static int remove_pool(jvm_allocator* this, jvm_allocation_pool* pool)
{
    if (pool->chunk_count > 1 || pool->chunks[0]->used)
    {
        JVM_ERROR(this, "Pool still has %u allocated chunks left", pool->chunk_count);
        return -2;
    }
    unsigned pos;
    for (pos = 0; pos < this->pool_count; ++pos)
    {
        if (this->pools[pos] == pool)
        {
            break;
        }
    }

    if (pos == this->pool_count)
    {
        JVM_ERROR(this, "Pool does not belong to the allocator");
        return -1;
    }

    if (this->pool_count - 1 - pos)
    {
        memmove(this->pools + pos, this->pools + pos + 1, sizeof(jvm_allocation_pool*) * (this->pool_count - 1 - pos));
    }
    this->pool_count -= 1;
    vkFreeMemory(this->device, pool->memory, allocator_vk_callbacks(this));
    jvm_free(this, pool->chunks);
    jvm_free(this, pool);
    return 0;
}

VkResult jvm_allocator_create(
        jvm_allocator_create_info create_info, const VkAllocationCallbacks* vk_allocation_callbacks,
        jvm_allocator** p_out)
{
    jvm_allocator_create_info info = create_info;
    if (!info.allocation_callbacks)
    {
        info.allocation_callbacks = &DEFAULT_ALLOC_CALLBACKS;
    }
    if (!info.error_callbacks)
    {
        info.error_callbacks = &DEFAULT_ERROR_CALLBACKS;
    }

    jvm_allocator* const this = info.allocation_callbacks->allocate(info.allocation_callbacks->state, sizeof(*this));
    if (!this)
    {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    this->allocation_callbacks = *info.allocation_callbacks;
    this->error_callbacks = *info.error_callbacks;
    this->has_vk_alloc = vk_allocation_callbacks != NULL;
    if (vk_allocation_callbacks)
    {
        this->vk_allocation_callbacks = *vk_allocation_callbacks;
    }

    this->device = info.device;

    this->pool_capacity = 0;
    this->pool_count = 0;
    this->pools = NULL;

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(info.physical_device, &props);
    this->min_map_alignment = props.limits.minMemoryMapAlignment;
    this->automatically_free_unused = info.automatically_free_unused;
    if (info.min_allocation_size == 0)
    {
        info.min_allocation_size = props.limits.nonCoherentAtomSize;
    }
    this->min_allocation_size = info.min_allocation_size;
    if (info.min_pool_size == 0)
    {
        info.min_pool_size = 1 << 20;
    }
    this->min_pool_size = info.min_pool_size;

    vkGetPhysicalDeviceMemoryProperties(info.physical_device, &this->memory_properties);

    *p_out = this;
    return VK_SUCCESS;
}

//  returns 0 when found, > 0 when no chunk was good, < 0 when memory allocation fails
int allocate_from_pool(
        jvm_allocator* allocator, jvm_allocation_pool* const pool, VkDeviceSize size, VkDeviceSize alignment,
        jvm_chunk** p_out)
{
    for (unsigned i = 0; i < pool->chunk_count; ++i)
    {
        jvm_chunk* chunk = pool->chunks[i];
        if (chunk->used == 1)
        {
            //  Chunk is in use
            continue;
        }
        VkDeviceSize padding = alignment - (
                chunk->chunk_offset & (
                        alignment - 1));   //  Based on assumption alignment is a power of two
        if (padding == alignment)
        {
            padding = 0;
        }
        if (chunk->size < padding + size)
        {
            //  Chunk is not large enough
            continue;
        }
        const VkDeviceSize left_over = chunk->size - (padding + size);
        if (left_over > allocator->min_allocation_size)
        {
            //  Chunk is large enough to split into two and only use one

            if (pool->chunk_count == pool->chunk_capacity)
            {
                const unsigned new_capacity = (pool->chunk_capacity ? pool->chunk_capacity : 8) << 1;
                jvm_chunk** const new_ptr = jvm_realloc(allocator, pool->chunks, sizeof(*pool->chunks) * new_capacity);
                if (!new_ptr)
                {
                    JVM_ERROR(allocator, "Could not (re-)allocate chunk list for the memory pool");
                    return -1;
                }
                pool->chunks = new_ptr;
                pool->chunk_capacity = new_capacity;
            }
            jvm_chunk* const new_allocation = jvm_alloc(allocator, sizeof(*new_allocation));
            if (!new_allocation)
            {
                JVM_ERROR(allocator, "Could not allocate memory for new chunk");
                return -1;
            }
            *new_allocation = (jvm_chunk)
                    {
                            .size = left_over,
                            .chunk_offset = chunk->chunk_offset + padding + size,
                            .pool = pool,
                            .memory = pool->memory,
                            .used = 0,
                            .padding = 0,
                            .mapped = 0,
                    };
            if (pool->chunk_count - 1 > i)
            {
                //  There are some chunks after this one
                memmove(
                        pool->chunks + i + 2, pool->chunks + i + 1,
                        sizeof(*pool->chunks) * (pool->chunk_count - 1 - i));
                assert(memcmp(pool->chunks + i + 1, pool->chunks + i + 2, sizeof(jvm_chunk*)) == 0);
            }
            pool->chunk_count += 1;
            pool->chunks[i + 1] = new_allocation;
            chunk->size = size + padding;
        }
        chunk->padding = padding;
        chunk->used = 1;

        *p_out = chunk;
        return 0;
    }
    return +1;
}

//  returns 0 when they don't merge, non-zero when they do
int merge_chunks(jvm_allocator* allocator, jvm_allocation_pool* pool, const unsigned i, const unsigned j)
{
    assert(i < pool->chunk_count);
    assert(j < pool->chunk_count);
    assert(i + 1 == j);

    if (pool->chunks[i]->used || pool->chunks[j]->used)
    {
        return 0;
    }

    jvm_chunk* const c1 = pool->chunks[i];
    jvm_chunk* const c2 = pool->chunks[j];

    assert(c1->chunk_offset + c1->size == c2->chunk_offset);

    c1->size += c2->size;
    memmove(pool->chunks + j, pool->chunks + j + 1, sizeof(*pool->chunks) * (pool->chunk_count - j - 1));
    pool->chunk_count -= 1;
    jvm_free(allocator, c2);

    return 1;
}

//  returns 0 when successful, > 0 when chunk is not from pool
int deallocate_from_pool(jvm_allocator* allocator, jvm_allocation_pool* const pool, jvm_chunk* chunk)
{
    assert(chunk->pool == pool);
    unsigned idx;
    for (idx = 0; idx < pool->chunk_count; ++idx)
    {
        if (pool->chunks[idx] == chunk)
        {
            break;
        }
    }
    if (idx == pool->chunk_count)
    {
        return -1;
    }
    chunk->used = 0;
    chunk->padding = 0;

    //  Merge with blocks after
    while ((idx < pool->chunk_count - 1))
    {
        assert(pool->chunks[idx]->chunk_offset + pool->chunks[idx]->size == pool->chunks[idx + 1]->chunk_offset);
        if (!merge_chunks(allocator, pool, idx, idx + 1))
        {
            break;
        }
    }

    //  Merge with blocks before
    while (idx)
    {
        assert(pool->chunks[idx - 1]->chunk_offset + pool->chunks[idx - 1]->size == pool->chunks[idx]->chunk_offset);
        if (!merge_chunks(allocator, pool, idx - 1, idx))
        {
            break;
        }
        idx -= 1;
    }

    return 0;
}

VkResult jvm_allocate(
        jvm_allocator* allocator, VkDeviceSize size, VkDeviceSize alignment, uint32_t type_bits,
        VkMemoryPropertyFlags desired_flags, VkMemoryPropertyFlags undesired_flags, jvm_chunk** p_out
#ifdef JVM_TRACK_ALLOCATIONS
        ,const char* file, int line
#endif
)
{
    if (size < allocator->min_allocation_size)
    {
        //  Should be at least this size
        size = allocator->min_allocation_size;
    }

    if (size < alignment)
    {
        size = alignment;
    }

    //  Reset scores
    const uint32_t mem_type_count = allocator->memory_properties.memoryTypeCount;
    int64_t scores[VK_MAX_MEMORY_TYPES] = { 0 };
    for (unsigned i = 0; i < mem_type_count; ++i)
    {
        scores[i] = 1;
    }
    //  Check for unwanted flags
    unsigned valid_count = 0;
    if (undesired_flags)
    {
        for (unsigned i = 0; i < mem_type_count; ++i)
        {
            if (allocator->memory_properties.memoryTypes[i].propertyFlags & undesired_flags)
            {
                scores[i] = 0;
            }
            valid_count += 1;
        }
        if (valid_count == 0)
        {
            JVM_ERROR(allocator, "Out of %u available memory types, none contained none of the undesired flags",
                      mem_type_count);
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
    }

    //  Check for allowed memory types
    for (unsigned i = 0; i < mem_type_count; ++i)
    {
        //  If the type does not have the flag bit set, then it can not be used
        if (!(type_bits & (1 << i)))
        {
            scores[i] = 0;
        }
    }

    valid_count = 0;
    for (unsigned i = 0; i < mem_type_count; ++i)
    {
        if (scores[i] == -1)
        {
            //  Has undesired flags
            continue;
        }
        if ((allocator->memory_properties.memoryTypes[i].propertyFlags & desired_flags) == desired_flags)
        {
            //  Has (at least) all desired flags
            scores[i] *= (int64_t) (
                    allocator->memory_properties.memoryHeaps[allocator->memory_properties.memoryTypes[i].heapIndex].size
                            >> 10);
            valid_count += (scores[i] != 0);
        }
    }
    if (valid_count == 0)
    {
        JVM_ERROR(allocator, "There was no memory type with desired memory flags");
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }

    int64_t best_score = 0;
    uint32_t idx = mem_type_count;
    for (unsigned i = 0; i < mem_type_count; ++i)
    {
        if (scores[i] > best_score)
        {
            best_score = scores[i];
            idx = (uint32_t) i;
        }
    }
    if (best_score == 0)
    {
        JVM_ERROR(allocator, "There was no available memory type to support allocation given the nature of the allocation,"
                             " the desired, and the undesired flags");
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }

    //  Check for need to map
    if ((allocator->memory_properties.memoryTypes[idx].propertyFlags & desired_flags) & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
    {
        if (alignment < allocator->min_map_alignment)
        {
            alignment = allocator->min_map_alignment;
        }
        if (size < alignment)
        {
            size = alignment;
        }
    }

    for (unsigned i = 0; i < allocator->pool_count; ++i)
    {
        jvm_allocation_pool* const pool = allocator->pools[i];
        if (pool->memory_type_index != idx)
        {
            //  Not correct type
            continue;
        }

        jvm_chunk* allocation;
        const int alloc_res = allocate_from_pool(allocator, pool, size, alignment, &allocation);
        if (alloc_res == 0)
        {
            //  Allocating from the pool was possible
#ifdef JVM_TRACK_ALLOCATIONS
            allocation->file = file;
            allocation->line = line;
#endif
            *p_out = allocation;
            return VK_SUCCESS;
        }
        else if (alloc_res < 0)
        {
            //  Could not allocate memory for pool internally
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
    }

    //  No pool was good enough, time to allocate a new one
    const VkDeviceSize new_pool_size = allocator->min_pool_size < size ? size : allocator->min_pool_size;
    VkResult vk_result = create_new_pool(
            allocator,
            new_pool_size,
            idx, allocator->memory_properties.memoryTypes[idx]);
    if (vk_result != VK_SUCCESS)
    {
        JVM_ERROR(allocator, "Could not allocate new memory pool of size %zu", (size_t) new_pool_size);
        return vk_result;
    }

    jvm_chunk* allocation;
    const int alloc_res = allocate_from_pool(
            allocator, allocator->pools[allocator->pool_count - 1], size, alignment, &allocation);
    assert(alloc_res <= 0);
    if (alloc_res != 0)
    {
        //  Could not allocate memory for pool internally
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    //  Allocating from the pool was possible
#ifdef JVM_TRACK_ALLOCATIONS
    allocation->file = file;
    allocation->line = line;
#endif
    *p_out = allocation;
    return VK_SUCCESS;
}

VkResult jvm_buffer_create(
        jvm_allocator* allocator, const VkBufferCreateInfo* create_info, VkMemoryPropertyFlags desired_flags,
        VkMemoryPropertyFlags undesired_flags, VkBool32 dedicated, jvm_buffer_allocation** p_out
#ifdef JVM_TRACK_ALLOCATIONS
        ,const char* file, int line
#endif
)
{
    jvm_buffer_allocation* const this = jvm_alloc(allocator, sizeof(*this));
    if (!this)
    {
        JVM_ERROR(allocator, "Could not allocate memory for buffer allocation");
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }

    VkBuffer buffer;
    VkResult vk_result = vkCreateBuffer(allocator->device, create_info, allocator_vk_callbacks(allocator), &buffer);
    if (vk_result != VK_SUCCESS)
    {
        JVM_ERROR(allocator, "Could not create new buffer: call to vkCreateBuffer failed");
        jvm_free(allocator, this);
        return vk_result;
    }

    VkMemoryRequirements mem_req;
    vkGetBufferMemoryRequirements(allocator->device, buffer, &mem_req);

    vk_result = !dedicated ? jvm_allocate(
            allocator,
            mem_req.size,
            mem_req.alignment, mem_req.memoryTypeBits,
            desired_flags,
            undesired_flags,
            &this->allocation
#ifdef JVM_TRACK_ALLOCATIONS
            ,file, line
#endif
            )
                          : jvm_allocate_dedicated(
                    allocator,
                    mem_req.size,
                    mem_req.alignment, mem_req.memoryTypeBits,
                    desired_flags,
                    undesired_flags,
                    &this->allocation
#ifdef JVM_TRACK_ALLOCATIONS
                    ,file, line
#endif
            );
    if (vk_result != VK_SUCCESS)
    {
        JVM_ERROR(allocator, "Could not allocate memory required for the buffer");
        vkDestroyBuffer(allocator->device, buffer, allocator_vk_callbacks(allocator));
        jvm_free(allocator, this);
        return vk_result;
    }
    vk_result = vkBindBufferMemory(
            allocator->device, buffer, this->allocation->memory,
            this->allocation->chunk_offset + this->allocation->padding);
    if (vk_result != VK_SUCCESS)
    {
        JVM_ERROR(allocator, "Could not bind memory to buffer");
        jvm_deallocate(allocator, this->allocation);
        vkDestroyBuffer(allocator->device, buffer, allocator_vk_callbacks(allocator));
        jvm_free(allocator, this);
        return vk_result;
    }
    this->buffer = buffer;
    this->allocator = allocator;

    *p_out = this;
    return VK_SUCCESS;
}

VkResult jvm_deallocate(jvm_allocator* allocator, jvm_chunk* chunk)
{
    jvm_allocation_pool* const pool = chunk->pool;
    const int dealloc_res = deallocate_from_pool(allocator, pool, chunk);
    if (dealloc_res < 0)
    {
        JVM_ERROR(allocator, "Could not deallocate chunk");
        return VK_ERROR_UNKNOWN;
    }
    if (pool->chunk_count == 1 && allocator->automatically_free_unused)
    {
        const int remove_res = remove_pool(allocator, pool);
        if (remove_res < 0)
        {
            JVM_ERROR(allocator, "Could not remove pool from allocator");
        }
    }

    return VK_SUCCESS;
}

VkResult map_pool_memory(jvm_allocator* allocator, jvm_allocation_pool* pool, uint8_t** p_ptr, int* p_first)
{
    if (pool->map_count)
    {
        *p_first = 0;
        pool->map_count += 1;
        *p_ptr = pool->map_ptr;
        return VK_SUCCESS;
    }

    VkResult res = vkMapMemory(allocator->device, pool->memory, 0, pool->size, 0, &pool->map_ptr);
    if (res != VK_SUCCESS)
    {
        JVM_ERROR(allocator, "Could not map memory pool to host memory");
        return res;
    }

    pool->map_count += 1;
    *p_ptr = pool->map_ptr;
    *p_first = 1;

    return VK_SUCCESS;
}

VkResult unmap_pool_memory(jvm_allocator* allocator, jvm_allocation_pool* pool, int* p_last)
{
    if (!pool->map_count)
    {
        JVM_ERROR(allocator, "A pool which was not mapped was attempted to be unmapped");
        return VK_ERROR_UNKNOWN;
    }
    pool->map_count -= 1;
    if (pool->map_count)
    {
        *p_last = 0;
        return VK_SUCCESS;
    }

    vkUnmapMemory(allocator->device, pool->memory);
    pool->map_ptr = NULL;
    *p_last = 1;
    return VK_SUCCESS;
}

VkResult jvm_chunk_map(jvm_allocator* allocator, jvm_chunk* chunk, size_t* p_size, void** p_out)
{
    if (chunk->mapped)
    {
        //  Should not be already mapped
        return VK_ERROR_MEMORY_MAP_FAILED;
    }
    uint8_t* pool_ptr;
    int first_map;
    VkResult res = map_pool_memory(allocator, chunk->pool, &pool_ptr, &first_map);
    if (res != VK_SUCCESS)
    {
        JVM_ERROR(allocator, "Could not map pool memory");
        return res;
    }
    chunk->mapped = 1;
    *p_out = pool_ptr + chunk->chunk_offset + chunk->padding;
    *p_size = chunk->size - chunk->padding;
    if (!first_map)
    {
        //  There was no actual call to vkMapMemory, so manually invalidate memory region
        return jvm_chunk_mapped_invalidate(allocator, chunk);
    }

    return VK_SUCCESS;
}

VkResult jvm_chunk_unmap(jvm_allocator* allocator, jvm_chunk* chunk)
{
    if (!chunk->mapped)
    {
        //  Should not be already mapped
        return VK_ERROR_MEMORY_MAP_FAILED;
    }
    int last_unmap;
    VkResult res = unmap_pool_memory(allocator, chunk->pool, &last_unmap);
    if (res == VK_SUCCESS)
    {
        chunk->mapped = 0;
        if (!last_unmap)
        {
            //  The unmapping did not actually call vkUnmapMemory, so manually flush all changes to memory
            return jvm_chunk_mapped_flush(allocator, chunk);
        }
    }
    return res;
}

VkResult jvm_buffer_map(jvm_buffer_allocation* buffer_allocation, size_t* p_size, void** p_out)
{
    return jvm_chunk_map(buffer_allocation->allocator, buffer_allocation->allocation, p_size, p_out);
}

VkResult jvm_buffer_unmap(jvm_buffer_allocation* buffer_allocation)
{
    return jvm_chunk_unmap(buffer_allocation->allocator, buffer_allocation->allocation);
}

VkResult jvm_image_map(jvm_image_allocation* image_allocation, size_t* p_size, void** p_out)
{
    return jvm_chunk_map(image_allocation->allocator, image_allocation->allocation, p_size, p_out);
}

VkResult jvm_image_unmap(jvm_image_allocation* image_allocation)
{
    return jvm_chunk_unmap(image_allocation->allocator, image_allocation->allocation);
}

VkResult jvm_chunk_mapped_flush(jvm_allocator* allocator, jvm_chunk* chunk)
{
    VkMappedMemoryRange range =
            {
                    .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
                    .memory = chunk->pool->memory,
                    .offset = chunk->chunk_offset,
                    .size = chunk->size,
            };
    return vkFlushMappedMemoryRanges(allocator->device, 1, &range);
}

VkResult jvm_chunk_mapped_invalidate(jvm_allocator* allocator, jvm_chunk* chunk)
{
    VkMappedMemoryRange range =
            {
                    .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
                    .memory = chunk->pool->memory,
                    .offset = chunk->chunk_offset,
                    .size = chunk->size,
            };
    return vkInvalidateMappedMemoryRanges(allocator->device, 1, &range);
}

VkResult jvm_allocate_dedicated(
        jvm_allocator* allocator, VkDeviceSize size, VkDeviceSize alignment, uint32_t type_bits,
        VkMemoryPropertyFlags desired_flags, VkMemoryPropertyFlags undesired_flags, jvm_chunk** p_out
#ifdef JVM_TRACK_ALLOCATIONS
        ,const char* file, int line
#endif
)
{
    if (size < allocator->min_allocation_size)
    {
        //  Should be at least this size
        size = allocator->min_allocation_size;
    }

    if (size < alignment)
    {
        size = alignment;
    }

    //  Reset scores
    const uint32_t mem_type_count = allocator->memory_properties.memoryTypeCount;
    int64_t scores[VK_MAX_MEMORY_TYPES] = { 0 };
    //  Check for unwanted flags
    unsigned valid_count = 0;
    if (undesired_flags)
    {
        for (unsigned i = 0; i < mem_type_count; ++i)
        {
            if (allocator->memory_properties.memoryTypes[i].propertyFlags & undesired_flags)
            {
                scores[i] = -1;
            }
            valid_count += 1;
        }
        if (valid_count == 0)
        {
            JVM_ERROR(allocator, "Out of %u available memory types, none contained none of the undesired flags",
                      mem_type_count);
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
    }

    //  Check for allowed memory types
    for (unsigned i = 0; i < mem_type_count; ++i)
    {
        //  If the type does not have the flag bit set, then it can not be used
        if (!(type_bits & (1 << i)))
        {
            scores[i] = -2;
        }
    }

    valid_count = 0;
    for (unsigned i = 0; i < mem_type_count; ++i)
    {
        if (scores[i] == -1)
        {
            //  Has undesired flags
            continue;
        }
        if ((allocator->memory_properties.memoryTypes[i].propertyFlags & desired_flags) == desired_flags)
        {
            //  Has (at least) all desired flags
            valid_count += 1;
            scores[i] = (int64_t) (
                    allocator->memory_properties.memoryHeaps[allocator->memory_properties.memoryTypes[i].heapIndex].size
                            >> 10);
        }
    }
    if (valid_count == 0)
    {
        JVM_ERROR(allocator, "There was no memory type with desired memory flags");
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }

    int64_t best_score = 0;
    uint32_t idx = mem_type_count;
    for (unsigned i = 0; i < mem_type_count; ++i)
    {
        if (scores[i] > best_score)
        {
            best_score = scores[i];
            idx = (uint32_t) i;
        }
    }
    if (best_score == 0)
    {
        JVM_ERROR(allocator, "There was no available memory type to support allocation given the nature of the allocation,"
                             " the desired, and the undesired flags");
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }

    //  Check for need to map
    if ((allocator->memory_properties.memoryTypes[idx].propertyFlags & desired_flags) & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
    {
        if (alignment < allocator->min_map_alignment)
        {
            alignment = allocator->min_map_alignment;
        }
        if (size < alignment)
        {
            size = alignment;
        }
    }

    //  Dedicated allocation requires a new pool
    const VkDeviceSize new_pool_size = size;
    VkResult vk_result = create_new_pool(
            allocator,
            new_pool_size,
            idx, allocator->memory_properties.memoryTypes[idx]);
    if (vk_result != VK_SUCCESS)
    {
        JVM_ERROR(allocator, "Could not allocate new memory pool of size %zu", (size_t) new_pool_size);
        return vk_result;
    }

    jvm_chunk* allocation;
    const int alloc_res = allocate_from_pool(
            allocator, allocator->pools[allocator->pool_count - 1], size, alignment, &allocation);
    assert(alloc_res <= 0);
    if (alloc_res != 0)
    {
        //  Could not allocate memory for pool internally
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    //  Allocating from the pool was possible
#ifdef JVM_TRACK_ALLOCATIONS
    allocation->file = file;
    allocation->line = line;
#endif
    *p_out = allocation;
    return VK_SUCCESS;
}

VkResult jvm_buffer_mapped_flush(jvm_buffer_allocation* buffer_allocation)
{
    return jvm_chunk_mapped_flush(buffer_allocation->allocator, buffer_allocation->allocation);
}

VkResult jvm_buffer_mapped_invalidate(jvm_buffer_allocation* buffer_allocation)
{
    return jvm_chunk_mapped_invalidate(buffer_allocation->allocator, buffer_allocation->allocation);
}

VkResult jvm_image_mapped_flush(jvm_image_allocation* image_allocation)
{
    return jvm_chunk_mapped_flush(image_allocation->allocator, image_allocation->allocation);
}

VkResult jvm_image_mapped_invalidate(jvm_image_allocation* image_allocation)
{
    return jvm_chunk_mapped_invalidate(image_allocation->allocator, image_allocation->allocation);
}

VkResult jvm_buffer_destroy(jvm_buffer_allocation* buffer_allocation)
{
    jvm_allocator* const allocator = buffer_allocation->allocator;
    vkDestroyBuffer(allocator->device, buffer_allocation->buffer, allocator_vk_callbacks(allocator));
    jvm_chunk* const chunk = buffer_allocation->allocation;
    if (chunk->mapped)
    {
        (void) jvm_chunk_unmap(allocator, chunk);
    }
    return jvm_deallocate(allocator, chunk);
}

VkResult jvm_image_create(
        jvm_allocator* allocator, const VkImageCreateInfo* create_info, VkMemoryPropertyFlags desired_flags,
        VkMemoryPropertyFlags undesired_flags, VkBool32 dedicated, jvm_image_allocation** p_out
#ifdef JVM_TRACK_ALLOCATIONS
        ,const char* file, int line
#endif
)
{
    jvm_image_allocation* const this = jvm_alloc(allocator, sizeof(*this));
    if (!this)
    {
        JVM_ERROR(allocator, "Could not allocate memory for image allocation");
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }

    VkImage img;
    VkResult vk_result = vkCreateImage(allocator->device, create_info, allocator_vk_callbacks(allocator), &img);
    if (vk_result != VK_SUCCESS)
    {
        JVM_ERROR(allocator, "Could not create new image");
        jvm_free(allocator, this);
        return vk_result;
    }

    VkMemoryRequirements mem_req;
    vkGetImageMemoryRequirements(allocator->device, img, &mem_req);

    vk_result = !dedicated ? jvm_allocate(
            allocator,
            mem_req.size,
            mem_req.alignment, mem_req.memoryTypeBits,
            desired_flags,
            undesired_flags,
            &this->allocation
#ifdef JVM_TRACK_ALLOCATIONS
            ,file, line
#endif
            )
                          : jvm_allocate_dedicated(
                    allocator,
                    mem_req.size,
                    mem_req.alignment, mem_req.memoryTypeBits,
                    desired_flags,
                    undesired_flags,
                    &this->allocation
#ifdef JVM_TRACK_ALLOCATIONS
                    ,file, line
#endif
            );
    if (vk_result != VK_SUCCESS)
    {
        JVM_ERROR(allocator, "Could not allocate memory required for the image");
        vkDestroyImage(allocator->device, img, allocator_vk_callbacks(allocator));
        jvm_free(allocator, this);
        return vk_result;
    }
    vk_result = vkBindImageMemory(
            allocator->device, img, this->allocation->memory,
            this->allocation->chunk_offset + this->allocation->padding);
    if (vk_result != VK_SUCCESS)
    {
        JVM_ERROR(allocator, "Could not bind memory to image");
        jvm_deallocate(allocator, this->allocation);
        vkDestroyImage(allocator->device, img, allocator_vk_callbacks(allocator));
        jvm_free(allocator, this);
        return vk_result;
    }
    this->image = img;
    this->allocator = allocator;

    *p_out = this;
    return VK_SUCCESS;
}

VkResult
jvm_image_destroy(jvm_image_allocation* image_allocation)
{
    jvm_allocator* const allocator = image_allocation->allocator;
    vkDestroyImage(allocator->device, image_allocation->image, allocator_vk_callbacks(allocator));
    jvm_chunk* const chunk = image_allocation->allocation;
    if (chunk->mapped)
    {
        (void) jvm_chunk_unmap(allocator, chunk);
    }
    return jvm_deallocate(allocator, chunk);
}

VkBuffer jvm_buffer_allocation_get_buffer(jvm_buffer_allocation* buffer_allocation)
{
    return buffer_allocation->buffer;
}

jvm_allocator* jvm_buffer_allocation_get_allocator(jvm_buffer_allocation* buffer_allocation)
{
    return buffer_allocation->allocator;
}

VkImage jvm_image_allocation_get_image(jvm_image_allocation* image_allocation)
{
    return image_allocation->image;
}

jvm_allocator* jvm_image_allocation_get_allocator(jvm_image_allocation* image_allocation)
{
    return image_allocation->allocator;
}

void jvm_allocator_free_unused(jvm_allocator* allocator)
{
#ifdef NDEBUG
    if (allocator->automatically_free_unused)
    {
        return;
    }
#endif
    for (unsigned i = 0; i < allocator->pool_count; ++i)
    {
        jvm_allocation_pool* const pool = allocator->pools[i];
        if (pool->chunk_count > 1 || pool->chunks[0]->used)
        {
            //  Chunk has more than one chunk, or it is in use
            continue;
        }
#ifndef NDEBUG
        if (allocator->automatically_free_unused)
        {
            JVM_ERROR(allocator, "Allocator should have freed block at index %u", i);
        }
#endif
        const int result = remove_pool(allocator, pool);
        (void) result;
        assert(result == 0);
    }
}

VkDeviceSize jvm_buffer_allocation_get_size(jvm_buffer_allocation* buffer_allocation)
{
    return buffer_allocation->allocation->size - buffer_allocation->allocation->padding;
}

VkDeviceSize jvm_image_allocation_get_size(jvm_image_allocation* image_allocation)
{
    return image_allocation->allocation->size - image_allocation->allocation->padding;
}
