#include "lib.h"

#define LINE_MAX 255

static bool blank(char c)
{
    return c == ' ' || c == '\t';
}

static int line_input(char *line, bool *previous_cr)
{
    size_t n = 0, overflow = 0;
    for (;;) {
        char c;
        long rc = read(0, &c, 1);
        if (rc < 0)
            return (int)rc;
        if (rc == 0)
            continue;
        bool swallow = *previous_cr && c == '\n';
        *previous_cr = c == '\r';
        if (swallow)
            continue;
        if (c == '\r' || c == '\n') {
            say("\n");
            line[n] = '\0';
            return overflow == 0 ? (int)n : FS_NAMETOOLONG;
        }
        if (c == '\003') {
            say("^C\n");
            return 0;
        }
        if (c == '\b' || c == '\177') {
            if (overflow != 0) {
                --overflow;
            } else if (n != 0) {
                --n;
                say("\b \b");
            }
        } else if ((unsigned char)c >= ' ' || c == '\t') {
            if (n == LINE_MAX) {
                ++overflow;
            } else {
                line[n++] = c;
                write(1, &c, 1);
            }
        }
    }
}

static int split(char *line, char **argv)
{
    char *out = line;
    int argc = 0;
    while (*line != '\0') {
        while (blank(*line))
            ++line;
        if (*line == '\0')
            break;
        if (argc == EXEC_MAX_ARGS)
            return -7;
        argv[argc++] = out;
        char quote = 0;
        while (*line != '\0' && (quote != 0 || !blank(*line))) {
            char c = *line++;
            if (c == quote) {
                quote = 0;
            } else if (quote == 0 && (c == '\'' || c == '"')) {
                quote = c;
            } else if (c == '\\' && quote != '\'' && *line != '\0') {
                *out++ = *line++;
            } else {
                *out++ = c;
            }
        }
        if (quote != 0)
            return FS_INVAL;
        while (blank(*line))
            ++line;
        *out++ = '\0';
    }
    argv[argc] = NULL;
    return argc;
}

static long list(const char *path)
{
    long fd = open(path, OPEN_RDONLY);
    if (fd < 0)
        return fd;
    struct fs_dirent entry;
    long rc;
    while ((rc = call(SYS_READDIR, fd, (long)&entry, 0)) == 1) {
        say(entry.name);
        say("\n");
    }
    close(fd);
    return rc;
}

static long cat(const char *path)
{
    long fd = open(path, OPEN_RDONLY);
    if (fd < 0)
        return fd;
    char data[FILE_IO_MAX];
    long n;
    while ((n = read(fd, data, sizeof(data))) > 0) {
        long written = write(1, data, n);
        if (written != n) {
            n = written < 0 ? written : FS_IO;
            break;
        }
    }
    close(fd);
    return n;
}

static long write_file(int argc, char **argv, unsigned flags)
{
    long fd = open(argv[1], OPEN_CREATE | OPEN_WRONLY | flags);
    if (fd < 0)
        return fd;
    long rc = 0;
    for (int i = 2; i < argc && rc == 0; ++i) {
        if (i > 2)
            rc = write_text(fd, " ");
        if (rc == 0)
            rc = write_text(fd, argv[i]);
    }
    if (rc == 0 && argc > 2)
        rc = write_text(fd, "\n");
    close(fd);
    return rc == 0 ? call(SYS_SYNC, 0, 0, 0) : rc;
}

static void error(const char *command, long code)
{
    char buf[24];
    size_t end = sizeof(buf) - 1, start = end;
    buf[end] = '\0';
    unsigned n = (unsigned)-code;
    do {
        buf[--start] = '0' + n % 10;
        n /= 10;
    } while (n != 0);
    buf[--start] = '-';
    say(command);
    say(": error ");
    say(buf + start);
    say("\n");
}

static long launch(char **argv)
{
    char path[5 + FS_NAME_MAX + 1];
    const char *program = argv[0];
    bool explicit_path = false;
    for (const char *s = program; *s != '\0'; ++s)
        explicit_path |= *s == '/';
    if (!explicit_path) {
        size_t n = length(program);
        if (n > FS_NAME_MAX)
            return FS_NAMETOOLONG;
        for (size_t i = 0; i < 5; ++i)
            path[i] = "/bin/"[i];
        for (size_t i = 0; i <= n; ++i)
            path[5 + i] = program[i];
        program = path;
    }
    long pid = call(SYS_FORK, 0, 0, 0);
    if (pid < 0)
        return pid;
    if (pid == 0) {
        long rc = call(SYS_EXEC, (long)program, (long)argv, 0);
        error(argv[0], rc);
        call(SYS_EXIT, 127, 0, 0);
        for (;;)
            call(SYS_SLEEP, 100, 0, 0);
    }
    int status;
    for (;;) {
        long child = call(SYS_WAIT, (long)&status, 0, 0);
        if (child < 0)
            return child;
        if (child == pid) {
            if (status != 0 && status != 127)
                error(argv[0], status < 0 ? status : -status);
            return 0;
        }
    }
}

static long execute(int argc, char **argv)
{
    const char *cmd = argv[0];
    if (equal(cmd, "help")) {
        say("help  ls [path]  cat path  cd [dir]  pwd  mkdir dir  rm path\n"
            "write path [text]  append path [text]  sync  echo [args]\n");
        return 0;
    }
    if (equal(cmd, "pwd")) {
        char path[FS_PATH_MAX + 1];
        if (argc != 1)
            return FS_INVAL;
        long rc = call(SYS_GETCWD, (long)path, sizeof(path), 0);
        if (rc < 0)
            return rc;
        say(path);
        say("\n");
        return 0;
    }
    if (equal(cmd, "ls"))
        return argc <= 2 ? list(argc == 2 ? argv[1] : ".") : FS_INVAL;
    if (equal(cmd, "cat"))
        return argc == 2 ? cat(argv[1]) : FS_INVAL;
    if (equal(cmd, "cd"))
        return argc <= 2 ? call(SYS_CHDIR, (long)(argc == 2 ? argv[1] : "/"), 0, 0) : FS_INVAL;
    if (equal(cmd, "mkdir") || equal(cmd, "rm")) {
        if (argc != 2)
            return FS_INVAL;
        long op = equal(cmd, "mkdir") ? SYS_MKDIR : SYS_UNLINK;
        long rc = call(op, (long)argv[1], 0, 0);
        return rc == 0 ? call(SYS_SYNC, 0, 0, 0) : rc;
    }
    if (equal(cmd, "write") || equal(cmd, "append")) {
        unsigned flags = equal(cmd, "write") ? OPEN_TRUNC : OPEN_APPEND;
        return argc >= 2 ? write_file(argc, argv, flags) : FS_INVAL;
    }
    if (equal(cmd, "sync"))
        return argc == 1 ? call(SYS_SYNC, 0, 0, 0) : FS_INVAL;
    return launch(argv);
}

int main(void)
{
    char line[LINE_MAX + 1];
    char *argv[EXEC_MAX_ARGS + 1];
    bool previous_cr = false;
    say("Seele shell. Type help for commands.\n");
    for (;;) {
        say("seele$ ");
        int n = line_input(line, &previous_cr);
        if (n < 0) {
            error("input", n);
        } else if (n != 0) {
            int argc = split(line, argv);
            if (argc < 0) {
                error("syntax", argc);
            } else if (argc != 0) {
                long rc = execute(argc, argv);
                if (rc < 0)
                    error(argv[0], rc);
            }
        }
    }
}
