#!/bin/sh
# Build the LK entry probe (in an arm64 Linux container: docker run --rm -v $PWD:/s -w /s ubuntu:24.04 ...)
set -e
cd "$(dirname "$0")"
gcc -O2 -ffreestanding -nostdlib -fno-pic -mgeneral-regs-only -mstrict-align -fno-stack-protector \
  -static -Wl,-e,_entry -Wl,--build-id=none \
  -Wl,-T,stub.lds -o pmic-reset.elf entry.S pmic-reset.c
# the stack (.bss) is part of the file: image_size covers it and LK copies the whole image
objcopy -O binary --set-section-flags .bss=alloc,load,contents pmic-reset.elf pmic-reset.bin
ls -la pmic-reset.bin
