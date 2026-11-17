QEMU := qemu-system-riscv64 -machine virt -bios default -m 128M -smp 2 -nographic
.PHONY: run debug
ifeq ($(PLATFORM),qemu-virt)
DISK ?= $(BUILD)/disk.img
DISK_FLAGS := -global virtio-mmio.force-legacy=false \
	-drive file=$(DISK),if=none,format=raw,id=seele-disk \
	-device virtio-blk-device,drive=seele-disk

$(BUILD)/disk.img:
	@mkdir -p $(@D)
	truncate -s 4M $@

run: build $(DISK)
	$(QEMU) $(DISK_FLAGS) -kernel $(BUILD)/kernel.elf
debug: build $(DISK)
	$(QEMU) $(DISK_FLAGS) -kernel $(BUILD)/kernel.elf -S -gdb tcp::1234
else
run debug:
	@echo '使用 image 生成 kernel.itb，再通过 U-Boot bootm 启动。'
endif
