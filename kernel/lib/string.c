#include <kernel/string.h>

void *memset(void *destination, int value, size_t count)
{
    uint8 *bytes = destination;

    for (size_t i = 0; i < count; ++i)
        bytes[i] = (uint8)value;

    return destination;
}

void *memcpy(void *destination, const void *source, size_t count)
{
    uint8 *out = destination;
    const uint8 *in = source;

    for (size_t i = 0; i < count; ++i)
        out[i] = in[i];
    return destination;
}
