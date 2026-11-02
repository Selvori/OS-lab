KERNEL_C := kernel/main.c kernel/lib/console.c kernel/lib/print.c \
	kernel/lib/string.c kernel/lock/spinlock.c kernel/mm/page.c kernel/mm/vm.c kernel/mm/user.c kernel/mm/heap.c kernel/proc/proc.c kernel/proc/syscall.c
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
