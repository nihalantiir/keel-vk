// VMA is a single-header library; exactly one .cpp defines
// VMA_IMPLEMENTATION. volk.h first so calls resolve to volk's pointers
// instead of the prototypes VK_NO_PROTOTYPES disables.
#define VMA_IMPLEMENTATION
#include <volk.h>
#include <vk_mem_alloc.h>
