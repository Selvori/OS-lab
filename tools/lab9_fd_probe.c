#include "lib.h"

#define require(condition) do { if (!(condition)) return fail(__LINE__); } while (0)
#define TEST_HEAP 0x3000L

static int fail(int line)
{
    char digits[] = { '0' + line / 100, '0' + line / 10 % 10, '0' + line % 10, '\0' };
    say("FD PROBE FAIL line ");
    say(digits);
    say("\n");
    return 1;
}

static long unlink(const char *path)
{
    return call(SYS_UNLINK, (long)path, 0, 0);
}

static long exec_probe(char **argv)
{
    return call(SYS_EXEC, (long)"/bin/fdprobe", (long)argv, 0);
}

static int resume(void)
{
    char c;
    char cwd[32];
    require(call(SYS_GETCWD, (long)cwd, sizeof(cwd), 0) == 5 && equal(cwd, "/work"));
    require(read(3, &c, 1) == 1 && c == 'b');
    require(call(SYS_MMAP, TEST_HEAP, 1, 0) == TEST_HEAP);
    require(*(volatile char *)TEST_HEAP == 0);
    require(call(SYS_MUNMAP, TEST_HEAP, 1, 0) == 0);
    close(3);
    say("FD EXEC PASS: cwd/offset retained, heap replaced\n");
    return 0;
}

static long mkdir(const char *path)
{
    return call(SYS_MKDIR, (long)path, 0, 0);
}

static bool exists_dir(const char *path)
{
    if (call(SYS_CHDIR, (long)path, 0, 0) != 0)
        return false;
    return call(SYS_CHDIR, (long)"..", 0, 0) == 0;
}

static int table_full(void)
{
    enum { WORKERS = 4 };
    char ready[] = "ready0";
    for (unsigned i = 0; i < WORKERS; ++i) {
        ready[5] = '0' + i;
        long child = call(SYS_FORK, 0, 0, 0);
        require(child >= 0);
        if (child == 0) {
            int count = 0;
            while (count < 13) {
                long fd = open("records", OPEN_RDONLY);
                if (fd == -23)
                    break;
                if (fd != count + 3)
                    break;
                ++count;
            }
            require(mkdir(ready) == 0);
            for (unsigned tries = 0; tries < 500; ++tries) {
                if (exists_dir("release")) {
                    call(SYS_EXIT, 100 + count, 0, 0);
                    return fail(__LINE__);
                }
                call(SYS_SLEEP, 1, 0, 0);
            }
            return fail(__LINE__);
        }
    }
    for (unsigned i = 0; i < WORKERS; ++i) {
        ready[5] = '0' + i;
        unsigned tries = 0;
        while (!exists_dir(ready)) {
            require(++tries < 400);
            call(SYS_SLEEP, 1, 0, 0);
        }
    }
    require(open("new-file", OPEN_CREATE | OPEN_WRONLY) == -23);
    require(mkdir("release") == 0);
    int total = 0;
    for (unsigned i = 0; i < WORKERS; ++i) {
        int status;
        require(call(SYS_WAIT, (long)&status, 0, 0) > 0);
        require(status >= 100 && status <= 113);
        total += status - 100;
        ready[5] = '0' + i;
        require(unlink(ready) == 0);
    }
    require(unlink("release") == 0);
    require(total == 48 && open("new-file", OPEN_RDONLY) == FS_NOENT);
    return 0;
}

int main(int argc, char **argv)
{
    require(argc == 2);
    if (equal(argv[1], "resume"))
        return resume();
    require(equal(argv[1], "two words"));
    require(write(2, "FD PROBE BEGIN\n", 15) == 15);

    char cwd[32];
    require(call(SYS_GETCWD, (long)cwd, sizeof(cwd), 0) == 5 && equal(cwd, "/work"));
    require(call(SYS_GETCWD, (long)cwd, 5, 0) == -34);
    require(call(SYS_GETCWD, 0, sizeof(cwd), 0) == -14);
    require(call(SYS_OPEN, 0, 0, 0) == -14);
    require(open(".", OPEN_WRONLY) == FS_ISDIR);
    require(open(".", 3) == FS_INVAL);
    require(unlink(".") == -16);

    long fd = open("records", OPEN_RDWR | OPEN_CREATE | OPEN_TRUNC);
    require(fd == 3);
    require(write(fd, (const void *)0x2ffe, 4) == -14);
    unsigned char bytes[FILE_IO_MAX];
    for (unsigned i = 0; i < sizeof(bytes); ++i)
        bytes[i] = (unsigned char)('a' + i % 26);
    for (unsigned i = 0; i < 2; ++i)
        require(write(fd, bytes, sizeof(bytes)) == sizeof(bytes));
    require(write(fd, bytes, 8) == 8);
    require(write(fd, bytes, sizeof(bytes) + 1) == FS_INVAL);
    struct fs_info info;
    require(call(SYS_FSTAT, fd, (long)&info, 0) == 0 && info.type == FS_FILE && info.size == 520);
    require(call(SYS_FSTAT, fd, 0x1000, 0) == -14);
    require(unlink("records") == -16);
    close(fd);

    fd = open("records", OPEN_RDONLY);
    long alias = call(SYS_DUP, fd, 0, 0);
    require(fd == 3 && alias == 4);
    require(read(fd, (void *)0x1000, 1) == -14);
    require(write(fd, bytes, 1) == -9);
    require(read(fd, bytes, 1) == 1 && bytes[0] == 'a');
    long child = call(SYS_FORK, 0, 0, 0);
    require(child >= 0);
    if (child == 0) {
        require(read(alias, bytes, 1) == 1 && bytes[0] == 'b');
        require(open("transient", OPEN_CREATE | OPEN_WRONLY) == 5);
        return 37;
    }
    int status;
    require(call(SYS_WAIT, (long)&status, 0, 0) == child && status == 37);
    require(read(fd, bytes, 1) == 1 && bytes[0] == 'c');
    require(unlink("transient") == 0);
    close(fd);
    require(read(alias, bytes, 1) == 1 && bytes[0] == 'd');
    close(alias);

    for (unsigned i = 3; i < 16; ++i)
        require(open("records", OPEN_RDONLY) == i);
    require(open("no-slot", OPEN_CREATE | OPEN_WRONLY) == -24);
    require(call(SYS_DUP, 1, 0, 0) == -24);
    for (unsigned i = 3; i < 16; ++i)
        close(i);
    require(open("no-slot", OPEN_RDONLY) == FS_NOENT);

    long dir = open(".", OPEN_RDONLY);
    require(dir == 3);
    require(call(SYS_READDIR, dir, 0, 0) == -14);
    struct fs_dirent entry;
    require(call(SYS_READDIR, dir, (long)&entry, 0) == 1 && equal(entry.name, "."));
    require(call(SYS_READDIR, dir, (long)&entry, 0) == 1 && equal(entry.name, ".."));
    require(call(SYS_READDIR, dir, (long)&entry, 0) == 1 && equal(entry.name, "note"));
    require(call(SYS_READDIR, dir, (long)&entry, 0) == 1 && equal(entry.name, "records"));
    require(call(SYS_READDIR, dir, (long)&entry, 0) == 0);
    close(dir);
    require(table_full() == 0);

    fd = open("records", OPEN_RDONLY);
    require(fd == 3 && read(fd, bytes, 1) == 1 && bytes[0] == 'a');
    require(call(SYS_MMAP, TEST_HEAP, 1, 0) == TEST_HEAP);
    *(volatile char *)TEST_HEAP = 'x';
    require(call(SYS_EXEC, (long)"/etc/motd", (long)argv, 0) == -8);
    require(call(SYS_EXEC, (long)"/bin/fdprobe", 1, 0) == -14);
    char lengthy[129];
    for (unsigned i = 0; i < sizeof(lengthy) - 1; ++i)
        lengthy[i] = 'x';
    lengthy[sizeof(lengthy) - 1] = '\0';
    char *bad[] = { lengthy, NULL };
    require(exec_probe(bad) == -7);
    require(*(volatile char *)TEST_HEAP == 'x');
    require(call(SYS_SYNC, 0, 0, 0) == 0);
    say("FD PASS: inheritance, dup, user ranges, limits and rollback\n");

    char *next[] = { "fdprobe", "resume", NULL };
    exec_probe(next);
    return fail(__LINE__);
}
