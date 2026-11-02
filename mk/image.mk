MKIMAGE ?= mkimage

$(BUILD)/fitgen: tools/fitgen.c
	@mkdir -p $(@D)
	$(HOSTCC) -std=c11 -Wall -Wextra -Werror $< -o $@
$(BUILD)/kernel.its: $(BUILD)/kernel.bin $(BUILD)/fitgen
	$(BUILD)/fitgen $(LOAD) > $@
$(BUILD)/kernel.itb: $(BUILD)/kernel.its mk/image.mk
	cd $(BUILD) && $(MKIMAGE) -f kernel.its kernel.itb
.PHONY: image
image: $(BUILD)/kernel.itb
