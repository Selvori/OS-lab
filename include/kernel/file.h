#ifndef SEELE_FILE_H
#define SEELE_FILE_H

#include <kernel/types.h>

#define NFD 16

struct proc;
void files_attach_console(struct proc *p);
void files_fork(struct proc *child, const struct proc *parent);
void files_drop(struct proc *p);
int proc_exec_image(struct proc *p, const char *path, uintptr_t argv);
bool proc_cwd_busy(uint32 ino);
long file_syscall(struct proc *p);

#endif
