#include <kernel/string.h>

#ifdef KERNEL_PAGE_MEMORY
#include <kernel/page.h>

/* The aligned page buffers may contain objects of any type. */
typedef uint64 page_word __attribute__((__may_alias__));
#endif

void *memset(void *destination, int value, size_t count)
{
#ifdef KERNEL_PAGE_MEMORY
    if (count == PAGE_SIZE && ((uintptr_t)destination & PAGE_MASK) == 0) {
        page_word *words = destination;
        uint64 pattern = (uint64)(uint8)value * UINT64_C(0x0101010101010101);

        for (size_t i = 0; i < PAGE_SIZE / sizeof(*words); ++i)
            words[i] = pattern;

        return destination;
    }
#endif

    uint8 *bytes = destination;

    for (size_t i = 0; i < count; ++i)
        bytes[i] = (uint8)value;

    return destination;
}

void *memcpy(void *destination, const void *source, size_t count)
{
#ifdef KERNEL_PAGE_MEMORY
    if (count == PAGE_SIZE &&
        (((uintptr_t)destination | (uintptr_t)source) & PAGE_MASK) == 0) {
        page_word *out = destination;
        const page_word *in = source;

        for (size_t i = 0; i < PAGE_SIZE / sizeof(*out); ++i)
            out[i] = in[i];

        return destination;
    }
#endif

    uint8 *out = destination;
    const uint8 *in = source;

    for (size_t i = 0; i < count; ++i)
        out[i] = in[i];
    return destination;
}
