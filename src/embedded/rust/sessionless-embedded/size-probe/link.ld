/* Minimal script so the probe links for a size measurement only. Not a real memory map. */
ENTRY(entry)
MEMORY { FLASH : ORIGIN = 0x08000000, LENGTH = 1M  RAM : ORIGIN = 0x20000000, LENGTH = 256K }
SECTIONS {
  .text : { *(.text .text.*) *(.rodata .rodata.*) } > FLASH
  .data : { *(.data .data.*) } > RAM AT > FLASH
  .bss  : { *(.bss .bss.*) *(COMMON) } > RAM
  /DISCARD/ : { *(.ARM.exidx*) *(.eh_frame*) }
}
