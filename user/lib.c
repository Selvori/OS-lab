#include "lib.h"

size_t length(const char *text)
{
    size_t n = 0;
    while (text[n] != '\0')
        ++n;
    return n;
}

bool equal(const char *a, const char *b)
{
    while (*a == *b) {
        if (*a == '\0')
            return true;
        ++a;
        ++b;
    }
    return false;
}

long write_text(long fd, const char *text)
{
    size_t count = length(text);
    while (count != 0) {
        size_t chunk = count > FILE_IO_MAX ? FILE_IO_MAX : count;
        long n = write(fd, text, chunk);
        if (n <= 0)
            return n == 0 ? FS_IO : n;
        count -= n;
        text += n;
    }
    return 0;
}

void say(const char *text)
{
    write_text(1, text);
}
