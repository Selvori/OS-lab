#include <stdio.h>
#include "crunch.h"

int main(void)
{
    printf("#define FIRST_SUM %uU\n#define SECOND_SUM %uU\n", crunch(73), crunch(149));
}
