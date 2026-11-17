#include "lib.h"

int main(int argc, char **argv)
{
    return argc == 2 && equal(argv[1], "work") ? 73 : 1;
}
