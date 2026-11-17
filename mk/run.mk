QEMU := qemu-system-riscv64 -machine virt -bios default -m 128M -smp 2 -nographic
.PHONY: run debug
ifeq ($(PLATFORM),qemu-virt)
DISK ?= $(BUILD)/seele-shell.img
DISK_FLAGS := -global virtio-mmio.force-legacy=false \
	-drive file=$(DISK),if=none,format=raw,id=seele-disk \
	-device virtio-blk-device,drive=seele-disk

USER_BINS := $(addprefix $(BUILD)/apps/,$(addsuffix .bin,$(USER_PROGRAMS)))
.PHONY: shell-image
shell-image: $(BUILD)/seele-shell.img

$(BUILD)/seele-shell.img: | $(USER_BINS)
	@mkdir -p $(@D)
	python3 tools/mkfs.py $@ $(foreach program,$(USER_PROGRAMS),--program $(program) $(BUILD)/apps/$(program).bin)

run: build $(DISK)
	$(QEMU) $(DISK_FLAGS) -kernel $(BUILD)/kernel.elf
debug: build $(DISK)
	$(QEMU) $(DISK_FLAGS) -kernel $(BUILD)/kernel.elf -S -gdb tcp::1234
else
run debug:
	@echo '使用 image 生成 kernel.itb，再通过 U-Boot bootm 启动。'
endif
