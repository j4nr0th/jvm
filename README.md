Simple C based Vulkan memory allocator, meant to help abstract sub-allocation of device memory for VkImage and VkBuffer. 

**NOT YET FULLY TESTED**

Requirements set for the module:
- return allocations (buffer/image, chunk_offset, size) which have desired usage and proper alignment
- reserve buffer/image memory with specified usage flags
- allow user to never need to interact with device memory or the device memory types
- can map image/buffer memory the way that vulkan would allow

Assumptions made by the module:
- alignment is a power of two
- alignment is equal or smaller than the allocation size
- memory is externally synchronized
