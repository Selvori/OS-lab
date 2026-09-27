#ifndef OSLAB_VM_H
#define OSLAB_VM_H

#include <asm/sv39.h>

void kernel_vm_init(void);
void kernel_vm_activate(void);
pagetable_t kernel_vm_root(void);
bool kernel_vm_active(void);

#endif
