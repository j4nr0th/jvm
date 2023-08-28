//
// Created by jan on 25.8.2023.
//

#ifndef JVM_JVM_H
#define JVM_JVM_H

#include <vulkan/vulkan.h>

/***********************************************************************************************************************
 *
 *
 *                                          Macros
 *
 *
 **********************************************************************************************************************/
#ifdef __GNUC__
#define JVM_INTERNAL_SYMBOL __attribute__((visibility("hidden")))
#define JVM_EXPORTED_SYMBOL __attribute__((visibility("default")))
#define JVM_IMPORTED_SYMBOL __attribute__((visibility("default")))
#endif

#if defined(_WIN32) || defined(__CYGWIN__)
#ifdef JVM_BUILD_SHARED
#define JVM_INTERNAL_SYMBOL
#define JVM_EXPORTED_SYMBOL __declspec(dllexport)
#define JVM_IMPORTED_SYMBOL __declspec(dllimport)
#else
#define JVM_INTERNAL_SYMBOL
#define JVM_EXPORTED_SYMBOL
#define JVM_IMPORTED_SYMBOL
#endif
#endif

#ifndef JVM_INTERNAL_SYMBOL
#warning "JVM_INTERNAL_SYMBOL macro was not defined, which probably means that the compiler attributes for symbols won't be correct"
#define JVM_INTERNAL_SYMBOL
#endif

#ifndef JVM_EXPORTED_SYMBOL
#warning "JVM_EXPORTED_SYMBOL macro was not defined, which probably means that the compiler attributes for symbols won't be correct"
#define JVM_EXPORTED_SYMBOL
#endif

#ifndef JVM_IMPORTED_SYMBOL
#warning "JVM_IMPORTED_SYMBOL macro was not defined, which probably means that the compiler attributes for symbols won't be correct"
#define JVM_IMPORTED_SYMBOL
#endif

#ifdef JVM_BUILD_LIBRARY
#define JVM_API JVM_EXPORTED_SYMBOL
#else
#define JVM_API JVM_IMPORTED_SYMBOL
#endif

/***********************************************************************************************************************
 *
 *
 *                                          Structs and typedefs
 *
 *
 **********************************************************************************************************************/

/**
 * Opaque handle to the allocator state. Linked to the VkDevice with which it was created.
 */
typedef struct jvm_allocator_T jvm_allocator;

/**
 * Struct which holds error report callback and its associated user pointer.
 */
typedef struct jvm_error_callbacks_T jvm_error_callbacks;

/**
 * Struct which holds allocation function pointers and associated user pointer.
 */
typedef struct jvm_allocation_callbacks_T jvm_allocation_callbacks;

/**
 * Struct which holds creation parameters for jvm_allocator.
 */
typedef struct jvm_allocator_create_info_T jvm_allocator_create_info;

/**
 * Opaque handle to a buffer allocation.
 */
typedef struct jvm_buffer_allocation_T jvm_buffer_allocation;

/**
 * Opaque handle to an image allocation.
 */
typedef struct jvm_image_allocation_T jvm_image_allocation;


struct jvm_allocation_callbacks_T
{
    /**
     * Callback used to allocate a new memory block of specified size.
     * @param state The jvm_allocation_callbacks::state pointer passed to the function.
     * @param size Size of the new memory block requested.
     * @return Pointer to the new memory block or NULL in case it was not possible to allocate it.
     */
    void* (* allocate)(void* state, uint64_t size);

    /**
     * Callback used to (re-)allocate a memory block to a new specified size.
     * @param state The jvm_allocation_callbacks::state pointer passed to the function
     * @param ptr NULL or a pointer to a previously allocated memory block
     * @param new_size New size, which the block should have
     * @return Pointer to the new (resized) memory block or NULL in case it was not possible to (re-)allocate it.
     */
    void* (* reallocate)(void* state, void* ptr, uint64_t new_size);

    /**
     * Callback used do deallocate memory after it is no longer needed.
     * @param state The jvm_allocation_callbacks::state pointer passed to the function.
     * @param ptr NULL or a pointer to a previously allocated memory block.
     */
    void (* free)(void* state, void* ptr);

    /**
     * Value passed to all callbacks within this struct as their first argument.
     */
    void* state;
};

struct jvm_error_callbacks_T
{
    /**
     * Callback used to report one or more errors, which may occur during a call to a function.
     * @param state The jvm_error_callbacks::state member.
     * @param msg Error message with more information about what went wrong.
     * @param file Name of the file where the error occurred.
     * @param line Line of in the file where the error was reported.
     * @param function Name of the function where the error occurred.
     */
    void (* report)(void* state, const char* msg, const char* file, int line, const char* function);

    /**
     * Value passed to jvm_error_callbacks::report whenever it is called.
     */
    void* state;
};

struct jvm_allocator_create_info_T
{
    /**
     * What is the minimum size of an individual memory pool. If set to 0, this is set to 4 MB.
     */
    VkDeviceSize min_pool_size;

    /**
     * Should unused pools be freed as soon as possible.
     */
    VkBool32 automatically_free_unused;

    /**
     * What is the smallest possible allocation size. If set to 0, it is set to VkPhysicalDeviceProperties::limits.nonCoherentAtomSize.
     */
    VkDeviceSize min_allocation_size;

    /**
     * Allocation callbacks to use. If set to NULL, default allocators (using malloc, realloc, and free) are used.
     */
    const jvm_allocation_callbacks* allocation_callbacks;

    /**
     * Error callbacks to use. If set to NULL, default callback (which uses fprintf to write to stderr) is used.
     */
    const jvm_error_callbacks* error_callbacks;

    /**
     * Vulkan logical device interface to use.
     */
    VkDevice device;

    /**
     * Physical Vulkan device to which the jvm_allocator_create_info::device corresponds to.
     */
    VkPhysicalDevice physical_device;
};


/***********************************************************************************************************************
 *
 *
 *                                          Allocator functions
 *
 *
 **********************************************************************************************************************/

/**
 * Creates a new Vulkan memory allocator.
 * @param create_info Struct which contains most of the creation parameters.
 * @param vk_allocation_callbacks Pointer to allocators to use for vulkan function calls. May be left NULL.
 * @param p_out Pointer which receives the created allocator.
 * @return VK_SUCCESS if successful, VK_ERROR_OUT_OF_HOST_MEMORY if it can not allocate required host memory.
 */
JVM_API
VkResult jvm_allocator_create(
        jvm_allocator_create_info create_info, const VkAllocationCallbacks* vk_allocation_callbacks,
        jvm_allocator** p_out);

/**
 * Destroys the provided Vulkan memory allocator, unmapping and freeing any associated memory. Will report an error for
 * each still unfree-d block.
 * @param allocator Allocator to destroy.
 */
JVM_API
void jvm_allocator_destroy(jvm_allocator* allocator);

/**
 * Frees unused memory pools. On allocators created with jvm_allocator_create_info::automatically_free_unused set to
 * non-zero, this does noting when not on debug build. On debug build, it will report any unfree-d pools as internal
 * errors
 * @param allocator Allocator for which to free unused pools.
 */
JVM_API
void jvm_allocator_free_unused(jvm_allocator* allocator);


/***********************************************************************************************************************
 *
 *
 *                                          Buffer related functions
 *
 *
 **********************************************************************************************************************/
/**
 * Creates a new buffer using vkCreateBuffer and binds the correct memory to it.
 * @param allocator Allocator to use for the allocation.
 * @param create_info Buffer creation info passed to vkCreateBuffer.
 * @param desired_flags Flags that are desired for the buffer memory to have. This is in addition to flags required
 * by the buffer.
 * @param undesired_flags Flags that the memory should not have. If these conflict with buffer required memory flags,
 * it will cause the function to fail with VK_ERROR_OUT_OF_DEVICE_MEMORY.
 * @param dedicated If non-zero, the allocation is made for this buffer only.
 * @param p_out Pointer to receive the create allocation.
 * @return VK_SUCCESS if successful, VK_ERROR_OUT_OF_HOST_MEMORY if it can not allocate required host memory,
 * return value of vkCreateBuffer if that fails, VK_ERROR_OUT_OF_DEVICE_MEMORY if undesired_flags conflict with flags
 * required by the buffer, return value of vkBindBufferMemory if that fails.
 */
JVM_API
VkResult jvm_buffer_create(
        jvm_allocator* allocator, const VkBufferCreateInfo* create_info, VkMemoryPropertyFlags desired_flags,
        VkMemoryPropertyFlags undesired_flags, VkBool32 dedicated, jvm_buffer_allocation** p_out);

/**
 * Destroys a buffer allocation, destroying the buffer and returning its device memory to its pool.
 * @param buffer_allocation Buffer allocation to free.
 * @return VK_SUCCESS if successful VK_ERROR_UNKNOWN an internal error occurred and buffer memory allocation
 * was not found in its proper pool.
 */
JVM_API
VkResult jvm_buffer_destroy(jvm_buffer_allocation* buffer_allocation);

/**
 * Attempts to map a provided buffer allocation to host memory. Behaviour depends on the memory flags from which it was
 * allocated.
 * @param buffer_allocation Buffer allocation to map.
 * @param p_size Pointer which receives the size of the mapped region.
 * @param p_out pointer which receives the pointer to the mapped memory.
 * @return VK_SUCCESS if successful, VK_ERROR_MEMORY_MAP_FAILED if chunk is already mapped, return value of vkMapMemory
 * if that fails, or the return value of vkFlushMappedMemoryRanges if that needed to be called and failed.
 */
JVM_API
VkResult jvm_buffer_map(jvm_buffer_allocation* buffer_allocation, size_t* p_size, void** p_out);

/**
 * Attempts to unmap a provided buffer allocation to host memory. Behaviour depends on the memory flags from which it was
 * allocated.
 * @param buffer_allocation Buffer allocation to unmap.
 * @return VK_SUCCESS if successful, VK_ERROR_MEMORY_MAP_FAILED if the chunk was not mapped before, or the return value
 * of vkInvalidateMappedMemoryRanges if that fails.
 */
JVM_API
VkResult jvm_buffer_unmap(jvm_buffer_allocation* buffer_allocation);

/**
 * Flushes mapped buffer to ensure changes made to it on the host are visible to the device.
 * @param buffer_allocation Mapped buffer allocation to flush.
 * @return VK_SUCCESS if successful, or return value of vkFlushMappedMemoryRanges if it fails.
 */
JVM_API
VkResult jvm_buffer_mapped_flush(jvm_buffer_allocation* buffer_allocation);

/**
 * Invalidates mapped buffer to ensure changes made to it from the device are visible to the host.
 * @param buffer_allocation Mapped buffer allocation to invalidate.
 * @return VK_SUCCESS if successful, or return value of vkInvalidateMappedMemoryRanges if it fails.
 */
JVM_API
VkResult jvm_buffer_mapped_invalidate(jvm_buffer_allocation* buffer_allocation);

/**
 * Returns the Vulkan handle to the buffer.
 * @param buffer_allocation Buffer allocation to get the handle from.
 * @return Handle to the buffer.
 */
JVM_API
VkBuffer jvm_buffer_allocation_get_buffer(jvm_buffer_allocation* buffer_allocation);

/**
 * Returns the allocator with which the buffer was allocated.
 * @param buffer_allocation Buffer allocation to get the allocator from.
 * @return Handle to the allocator.
 */
JVM_API
jvm_allocator* jvm_buffer_allocation_get_allocator(jvm_buffer_allocation* buffer_allocation);



/***********************************************************************************************************************
 *
 *
 *                                          Image related functions
 *
 *
 **********************************************************************************************************************/

/**
 * Creates a new image using vkCreateBuffer and binds the correct memory to it.
 * @param allocator Allocator to use for the allocation.
 * @param create_info Image creation info passed to vkCreateImage.
 * @param desired_flags Flags that are desired for the image memory to have. This is in addition to flags required
 * by the image.
 * @param undesired_flags Flags that the memory should not have. If these conflict with image required memory flags,
 * it will cause the function to fail with VK_ERROR_OUT_OF_DEVICE_MEMORY.
 * @param dedicated If non-zero, the allocation is made for this image only.
 * @param p_out Pointer to receive the create allocation.
 * @return VK_SUCCESS if successful, VK_ERROR_OUT_OF_HOST_MEMORY if it can not allocate required host memory,
 * return value of vkCreateBuffer if that fails, VK_ERROR_OUT_OF_DEVICE_MEMORY if undesired_flags conflict with flags
 * required by the image, return value of vkBindImageMemory if that fails.
 */
JVM_API
VkResult jvm_image_create(
        jvm_allocator* allocator, const VkImageCreateInfo* create_info, VkMemoryPropertyFlags desired_flags,
        VkMemoryPropertyFlags undesired_flags, VkBool32 dedicated, jvm_image_allocation** p_out);

/**
 * Destroys a image allocation, destroying the image and returning its device memory to its pool.
 * @param image_allocation Image allocation to free.
 * @return VK_SUCCESS if successful VK_ERROR_UNKNOWN an internal error occurred and image memory allocation
 * was not found in its proper pool.
 */
JVM_API
VkResult jvm_image_destroy(jvm_image_allocation* image_allocation);

/**
 * Attempts to map a provided image allocation to host memory. Behaviour depends on the memory flags from which it was
 * allocated.
 * @param image_allocation Image allocation to map.
 * @param p_size Pointer which receives the size of the mapped region.
 * @param p_out Pointer which receives the pointer to the mapped memory.
 * @return VK_SUCCESS if successful, VK_ERROR_MEMORY_MAP_FAILED if chunk is already mapped, return value of vkMapMemory
 * if that fails, or the return value of vkFlushMappedMemoryRanges if that needed to be called and failed.
 */
JVM_API
VkResult jvm_image_map(jvm_image_allocation* image_allocation, size_t* p_size, void** p_out);

/**
 * Attempts to unmap a provided image allocation to host memory. Behaviour depends on the memory flags from which it was
 * allocated.
 * @param image_allocation Image allocation to unmap.
 * @return VK_SUCCESS if successful, VK_ERROR_MEMORY_MAP_FAILED if the chunk was not mapped before, or the return value
 * of vkInvalidateMappedMemoryRanges if that fails.
 */
JVM_API
VkResult jvm_image_unmap(jvm_image_allocation* image_allocation);

/**
 * Flushes mapped image to ensure changes made to it on the host are visible to the device.
 * @param image_allocation Mapped image allocation to flush.
 * @return VK_SUCCESS if successful, or return value of vkFlushMappedMemoryRanges if it fails.
 */
JVM_API
VkResult jvm_image_mapped_flush(jvm_image_allocation* image_allocation);

/**
 * Invalidates mapped image to ensure changes made to it from the device are visible to the host.
 * @param image_allocation Mapped image allocation to invalidate.
 * @return VK_SUCCESS if successful, or return value of vkInvalidateMappedMemoryRanges if it fails.
 */
JVM_API
VkResult jvm_image_mapped_invalidate(jvm_image_allocation* image_allocation);

/**
 * Returns the Vulkan handle to the image.
 * @param image_allocation Image allocation to get the handle from.
 * @return Handle to the image.
 */
JVM_API
VkImage jvm_image_allocation_get_image(jvm_image_allocation* image_allocation);

/**
 * Returns the allocator with which the image was allocated.
 * @param image_allocation Image allocation to get the allocator from.
 * @return Handle to the allocator.
 */
JVM_API
jvm_allocator* jvm_image_allocation_get_allocator(jvm_image_allocation* image_allocation);


#endif //JVM_JVM_H
