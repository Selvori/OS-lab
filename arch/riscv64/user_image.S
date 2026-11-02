.section .user_image,"a",@progbits
.balign 8
.globl user_image_start, user_image_end
user_image_start:
.incbin "user.bin"
user_image_end:
