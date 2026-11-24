#include "lib.h"
#include "crunch.h"
#include "expected.h"

int main(void)
{
    long pids[2];
    unsigned reaped = 0;
    say("USER-PAIR BEGIN\n");
    for (int i = 0; i < 2; ++i) {
        long pid = call(SYS_FORK, 0, 0, 0);
        if (pid < 0)
            return 1;
        if (pid == 0) {
            long self = call(SYS_GETPID, 0, 0, 0);
            for (unsigned j = 0; j < 4096; ++j)
                if (call(SYS_GETPID, 0, 0, 0) != self)
                    return -1;
            say("PHASE COMPUTE\n");
#ifdef EXTENDED_OBSERVATION
            /* Lengthen the observation window across GDB attachments. */
            volatile unsigned seed = i == 0 ? 73U : 149U;
            unsigned expected = i == 0 ? FIRST_SUM : SECOND_SUM;
            for (unsigned round = 0; round < 8; ++round)
                if (crunch(seed) != expected)
                    return -1;
            return (int)expected;
#else
            return (int)crunch(i == 0 ? 73U : 149U);
#endif
        }
        pids[i] = pid;
    }
    for (int i = 0; i < 2; ++i) {
        int status = -1;
        long pid = call(SYS_WAIT, (long)&status, 0, 0);
        if (pid == pids[0] && (unsigned)status == FIRST_SUM)
            reaped |= 1U;
        else if (pid == pids[1] && (unsigned)status == SECOND_SUM)
            reaped |= 2U;
        else
            return 2;
    }
    if (reaped != 3 || call(SYS_WAIT, 0, 0, 0) != -10)
        return 3;
    say("USER-PAIR VERIFIED\n");
    return 0;
}
