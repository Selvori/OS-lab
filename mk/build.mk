KERNEL_C := kernel/main.c kernel/lib/console.c kernel/lib/print.c \
	kernel/lib/string.c kernel/lock/spinlock.c kernel/mm/page.c kernel/mm/vm.c kernel/mm/user.c kernel/mm/heap.c kernel/proc/proc.c kernel/proc/syscall.c \
	kernel/fs/block.c kernel/fs/fs.c kernel/fs/file.c kernel/proc/exec.c
ifneq ($(filter time counts,$(LIFECYCLE)),)
CPPFLAGS += -DLIFECYCLE_PROBE
KERNEL_C += kernel/proc/lifecycle_probe.c
ifeq ($(LIFECYCLE),counts)
CPPFLAGS += -DLIFECYCLE_COUNTS
endif
endif
SOURCES := $(KERNEL_C) $(ARCH_C) $(PLATFORM_C)
OBJECTS := $(addprefix $(BUILD)/,$(SOURCES:.c=.o) $(ARCH_S:.S=.o))
-include $(OBJECTS:.o=.d)

$(BUILD)/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
$(BUILD)/%.o: %.S
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@
$(BUILD)/kernel.ld: arch/$(ARCH)/linker/kernel.lds.S platform/$(PLATFORM)/platform.h arch/$(ARCH)/include/asm/boot.h
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) -E -P -x c -DLOAD=$(LOAD) $< -o $@
$(BUILD)/kernel.elf: $(OBJECTS) $(BUILD)/kernel.ld
	$(CC) $(CFLAGS) -nostdlib -static -Wl,--build-id=none,--no-relax,-T,$(BUILD)/kernel.ld $(OBJECTS) -lgcc -o $@
$(BUILD)/kernel.bin: $(BUILD)/kernel.elf
	$(OBJCOPY) -O binary $< $@
.PHONY: build
build: $(BUILD)/kernel.elf

# Build a position-fixed integer-only user program, then embed its raw code page.
$(BUILD)/user.elf: user/init.S user/user.ld include/kernel/syscall.h
	@mkdir -p $(@D)
	$(CC) -Iinclude -nostdlib -static -fno-pie -march=rv64imac -mabi=lp64 -mno-relax -Wl,--build-id=none,--no-relax,-T,user/user.ld user/init.S -o $@
$(BUILD)/user.bin: $(BUILD)/user.elf
	$(OBJCOPY) -O binary $< $@
$(BUILD)/arch/riscv64/user_image.o: arch/riscv64/user_image.S $(BUILD)/user.bin
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Wa,-I$(BUILD) -c $< -o $@

USER_PROGRAMS := sh echo
USER_APP_CFLAGS := -std=c11 -Os -Wall -Wextra -Werror -ffreestanding -fno-builtin \
	-fno-pie -fno-stack-protector -mno-relax -march=rv64imac -mabi=lp64
APP_DEPS := kernel/lib/string.c include/kernel/string.h user/lib.c user/lib.h user/start.S user/program.ld \
	include/kernel/file_abi.h include/kernel/fs.h include/kernel/syscall.h

$(BUILD)/apps/%.elf: user/%.c $(APP_DEPS)
	$(compile_user_program)

$(BUILD)/apps/fdprobe.elf: tools/lab9_fd_probe.c $(APP_DEPS)
	$(compile_user_program)

define compile_user_program
	@mkdir -p $(@D)
	$(CC) -Iinclude -Iuser $(USER_APP_CFLAGS) -nostdlib -static \
		-Wl,--build-id=none,--no-relax,-T,user/program.ld \
		user/start.S user/lib.c kernel/lib/string.c $< -o $@
endef

$(BUILD)/apps/%.bin: $(BUILD)/apps/%.elf
	$(OBJCOPY) -O binary $< $@

$(BUILD)/apps/lifebench.elf: tools/lifecycle_bench.c $(APP_DEPS) include/kernel/lifecycle_probe.h
	$(compile_user_program)

$(BUILD)/apps/lifejob.elf: tools/lifecycle_job.c $(APP_DEPS)
	$(compile_user_program)
