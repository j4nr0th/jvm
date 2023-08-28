//
// Created by jan on 26.8.2023.
//

#ifndef JVM_INTERNAL_H
#define JVM_INTERNAL_H

#include "../include/jvm.h"


//  Requirements:
//      - return allocations (buffer/image, chunk_offset, size) which have desired usage and proper alignment
//      - reserve buffer/image memory with specified usage flags
//      - allow user to never need to interact with device memory or the device memory types
//      - can map image/buffer memory the way that vulkan would allow
//
//  Assumptions:
//      - sharing mode is VK_SHARING_MODE_CONCURRENT
//      - alignment is a power of two
//      - alignment is equal or smaller than the allocation size
//      - memory is externally synchronized

typedef struct jvm_allocation_pool_T jvm_allocation_pool;
typedef struct jvm_chunk_T jvm_chunk;

struct jvm_chunk_T
{
    VkBool32 mapped;         //  zero if not mapped
    VkBool32 used;           //  zero if not in use
    VkDeviceSize size;           //  real size of the chunk (includes any padding and rounding)
    VkDeviceMemory memory;         //  memory handle of its pool
    VkDeviceSize chunk_offset;   //  where the chunk begins in the pool's memory
    VkDeviceSize padding;        //  how much to pad from chunk_offset to have proper alignment
    jvm_allocation_pool* pool;           //  what pool it belongs to
};

struct jvm_buffer_allocation_T
{
    jvm_allocator* allocator;  //  Allocator with which this was allocated with
    jvm_chunk* allocation; //  The underlying memory allocation chunk
    VkBuffer buffer;     //  Vulkan buffer handle bound to memory
};

struct jvm_image_allocation_T
{
    jvm_allocator* allocator;  //  Allocator with which this was allocated with
    jvm_chunk* allocation; //  The underlying memory allocation chunk
    VkImage image;      //  Vulkan image handle bound to memory
};

struct jvm_allocation_pool_T
{
    uint32_t memory_type_index;  //  Index of the memory type
    VkDeviceMemory memory;             //  Vulkan's memory handle
    unsigned chunk_count;        //  Number of chunks currently in the pool
    unsigned chunk_capacity;     //  Maximum number of chunks that can currently be held in the jvm_allocation_pool::chunks member
    unsigned map_count;          //  How many chunks in the pool are currently mapped
    void* map_ptr;            //  Pointer to the memory mapping
    jvm_chunk** chunks;             //  Array of all chunks in the pool. At no point in time should two adjacent ones be unused (merge them)
    VkMemoryType memory_type_info;   //  Memory type of the memory pool
    VkDeviceSize size;               //  Size of the pool
};
struct jvm_allocator_T
{
    jvm_allocation_callbacks allocation_callbacks;       //  Allocation callbacks and associated state
    jvm_error_callbacks error_callbacks;            //  Error report callback and associated state

    int has_vk_alloc;               //  non-zero if vulkan allocation callbacks were given
    VkAllocationCallbacks vk_allocation_callbacks;    //  allocation callbacks if they were given at creation
    VkPhysicalDevice physical_device;            //  physical device associated with the logical device
    VkDevice device;                     //  logical device interface to the Vulkan device
    VkPhysicalDeviceMemoryProperties memory_properties;          //  physical memory properties

    VkDeviceSize min_pool_size;              //  minimum size of individual pools
    VkBool32 automatically_free_unused;  //  if non-zero, a pool with only one unused chunk get freed ASAP
    VkDeviceSize min_allocation_size;        //  smallest memory allocation that can be made
    size_t min_map_alignment;          //  minimum alignment needed to be able to map memory

    unsigned pool_count;                 //  current number of memory pools
    unsigned pool_capacity;              //  maximum number of memory pools that can be put in the pool
    jvm_allocation_pool** pools;                      //  array of memory pools
};

//  General functions (internal use)

JVM_INTERNAL_SYMBOL
VkResult jvm_allocate(
        jvm_allocator* allocator, VkDeviceSize size, VkDeviceSize alignment, VkMemoryPropertyFlags desired_flags,
        VkMemoryPropertyFlags undesired_flags, jvm_chunk** p_out);

JVM_INTERNAL_SYMBOL
VkResult jvm_allocate_dedicated(
        jvm_allocator* allocator, VkDeviceSize size, VkDeviceSize alignment, VkMemoryPropertyFlags desired_flags,
        VkMemoryPropertyFlags undesired_flags, jvm_chunk** p_out);

JVM_INTERNAL_SYMBOL
VkResult jvm_deallocate(jvm_allocator* allocator, jvm_chunk* chunk);

JVM_INTERNAL_SYMBOL
VkResult jvm_chunk_map(jvm_allocator* allocator, jvm_chunk* chunk, size_t* p_size, void** p_out);

JVM_INTERNAL_SYMBOL
VkResult jvm_chunk_unmap(jvm_allocator* allocator, jvm_chunk* chunk);

JVM_INTERNAL_SYMBOL
VkResult jvm_chunk_mapped_flush(jvm_allocator* allocator, jvm_chunk* chunk);

JVM_INTERNAL_SYMBOL
VkResult jvm_chunk_mapped_invalidate(jvm_allocator* allocator, jvm_chunk* chunk);


JVM_INTERNAL_SYMBOL
void* jvm_alloc(const jvm_allocator* alc, uint64_t size);

JVM_INTERNAL_SYMBOL
void* jvm_realloc(const jvm_allocator* alc, void* ptr, uint64_t new_size);

JVM_INTERNAL_SYMBOL
void jvm_free(const jvm_allocator* alc, void* ptr);

JVM_INTERNAL_SYMBOL
extern const jvm_allocation_callbacks DEFAULT_ALLOC_CALLBACKS;

JVM_INTERNAL_SYMBOL
extern const jvm_error_callbacks DEFAULT_ERROR_CALLBACKS;

#ifdef __GNUC__

__attribute__((format(printf, 2, 6)))
#endif
JVM_INTERNAL_SYMBOL
void jvm_report_error(const jvm_allocator* alc, const char* fmt, const char* file, int line, const char* function, ...);

#ifndef JVM_ERROR
#define JVM_ERROR(alc, msg, ...) jvm_report_error((alc), (msg), __FILE__, __LINE__, __func__ __VA_OPT__(,) __VA_ARGS__)
#endif

#endif //JVM_INTERNAL_H
