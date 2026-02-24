/*
 * Linker script for WAMR PAL bare-metal ELF (VMPL1)
 * Based on gramine-svsm/pal/src/host/svsm/arch/x86_64/pal.lds
 *
 * Simplified for static linking (-static -nostdlib):
 *   - Removed dynamic linking sections (.dynamic, .got, .got.plt, .dynsym, etc.)
 *   - Kept only essential sections (.text, .rodata, .data, .bss)
 *
 * SVSM's build_from_elf() reads PT_LOAD segments and e_entry,
 * it does NOT check ET_DYN vs ET_EXEC. Using base address 0 for
 * consistency with the Gramine PAL.
 */

OUTPUT_FORMAT("elf64-x86-64", "elf64-x86-64", "elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(pal_start);

SECTIONS
{
  /* Read-only sections, merged into text segment */
  __text_start = .;
  . = SEGMENT_START("text-segment", 0) + SIZEOF_HEADERS;

  .text :
  {
    *(.text .text.*)
  }

  .rodata :
  {
    *(.rodata .rodata.*)
  }

  .eh_frame_hdr  : { *(.eh_frame_hdr) }
  .eh_frame      : ONLY_IF_RO { *(.eh_frame) }

  __text_end = .;

  /* Adjust address for the data segment */
  . = DATA_SEGMENT_ALIGN(CONSTANT(MAXPAGESIZE), CONSTANT(COMMONPAGESIZE));

  __data_start = .;

  .eh_frame      : ONLY_IF_RW { *(.eh_frame) }

  .data :
  {
    *(.data .data.*)
  }

  .bss :
  {
    *(.bss .bss.*)
    *(COMMON)
  }

  . = DATA_SEGMENT_END(.);
  __data_end = .;
}
