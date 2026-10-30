SECTIONS
{
	. = 0;
	.text : { *(.text.entry) *(.text*) }
	.rodata : { *(.rodata*) }
	.data : { *(.data*) }
	.bss : { *(.bss*) *(COMMON) }
	_end = .;
	/DISCARD/ : { *(.dynamic) *(.dynsym) *(.dynstr) *(.hash) *(.gnu.hash) *(.rela*) *(.comment) *(.note*) *(.eh_frame*) }
}
