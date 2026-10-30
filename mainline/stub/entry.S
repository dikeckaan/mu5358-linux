/*
 * An arm64 Image header in front of pmic-reset.c, so the U30 Pro's LK loads it as it loads a kernel. LK places the
 * kernel at a different address on every boot (0x80090000, 0x80610000, ...: uboot_log), so everything here is
 * position independent: the stack is the 64 KiB at the end of the image, found PC-relative.
 */
	.section .text.entry, "ax"
	.global _entry
_entry:
	b	start			/* code0 */
	.word	0			/* code1 */
	.quad	0			/* text_offset */
	.quad	_end - _entry		/* image_size */
	.quad	0x2			/* flags: little endian, 4 KiB pages */
	.quad	0, 0, 0			/* res2-res4 */
	.ascii	"ARMd"			/* magic */
	.word	0			/* res5 */
start:
	adr	x9, stack_top
	mov	sp, x9
	bl	pmic_reset
	b	.

	.section .bss.stack, "aw", %nobits
	.balign	16
	.space	0x10000
stack_top:
