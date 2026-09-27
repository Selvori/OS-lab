#include <kernel/string.h>

void *memset(void *destination, int value, size_t count)
{
    uint8 *bytes = destination;

    for (size_t i = 0; i < count; ++i)
        bytes[i] = (uint8)value;

    return destination;
}
