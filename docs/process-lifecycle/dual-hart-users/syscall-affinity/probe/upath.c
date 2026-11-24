#include "bounded_guest.h"
#include "crunch.h"
#include "expected.h"

#define CALLS 16384U

int main(int argc, char **argv)
{
    if (argc != 3 || !(equal(argv[1], "cpu") || equal(argv[1], "sys")) ||
        !(equal(argv[2], "1") || equal(argv[2], "2")))
        return 1;

    bool arithmetic = equal(argv[1], "cpu");
    unsigned children = (unsigned)(argv[2][0] - '0'), checked = 0;
    long pids[2];
    unsigned long start = now();
    for (unsigned i = 0; i < children; ++i) {
        long pid = call(SYS_FORK, 0, 0, 0);
        CHECK(pid >= 0);
        if (pid == 0) {
            if (arithmetic)
                return (int)crunch(i == 0 ? 73U : 149U);
            long self = call(SYS_GETPID, 0, 0, 0);
            for (unsigned j = 0; j < CALLS; ++j)
                if (call(SYS_GETPID, 0, 0, 0) != self)
                    return -1;
            return 42 + (int)i;
        }
        pids[i] = pid;
    }
    for (unsigned j = 0; j < children; ++j) {
        int status = -1;
        long pid = call(SYS_WAIT, (long)&status, 0, 0);
        unsigned i = pid == pids[0] ? 0U : 1U;
        CHECK(i < children && pids[i] == pid && !(checked & (1U << i)));
        unsigned expected = arithmetic ? (i == 0 ? FIRST_SUM : SECOND_SUM) : 42 + i;
        CHECK((unsigned)status == expected);
        checked |= 1U << i;
    }
    unsigned long ticks = now() - start;
    CHECK(checked == (1U << children) - 1 && call(SYS_WAIT, 0, 0, 0) == -10);
    ROW("PATH OK", children, arithmetic ? ROUNDS : CALLS, ticks);
    return 0;
}
