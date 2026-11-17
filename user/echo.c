#include "lib.h"

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; ++i) {
        if (i > 1)
            say(" ");
        say(argv[i]);
    }
    say("\n");
    return 0;
}
