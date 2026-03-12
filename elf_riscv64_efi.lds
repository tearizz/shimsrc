/* SPDX-License-Identifier: GPL-2.0+ OR BSD-2-Clause */
/*
 * Linker script for RISC-V 64-bit EFI images using the -O binary approach
 * (crt0-efi-riscv64-local.o embeds the PE/COFF MZ header in .text.head)
 */

OUTPUT_FORMAT("elf64-littleriscv", "elf64-littleriscv", "elf64-littleriscv")
OUTPUT_ARCH(riscv)
ENTRY(_start)
SECTIONS
{
  .text 0x0 : {
    *(.text.head)
    . = ALIGN(4096);
    _text = .;
    *(.text)
    *(.text.*)
    *(.gnu.linkonce.t.*)
    *(.plt)
    _evtext = .;
    . = ALIGN(4096);
  }
  _etext = .;
  _text_size = . - _text;
  _text_vsize = _evtext - _text;

  . = ALIGN(4096);
  .data.ident : {
    *(.data.ident)
  }
  . = ALIGN(4096);
  .sbatlevel : {
    *(.sbatlevel)
  }
  . = ALIGN(4096);
  .dynamic  : { *(.dynamic) }
  . = ALIGN(4096);
  .data :
  {
   _data = .;
   *(.sdata)
   *(.data)
   *(.data1)
   *(.data.*)
   *(.got.plt)
   *(.got)

   . = ALIGN(16);
   __init_array_start = .;
   *(SORT(.init_array.*))
   *(.init_array)
   __init_array_end = .;
   . = ALIGN(16);
   __CTOR_LIST__ = .;
   *(SORT(.ctors.*))
   *(.ctors)
   __CTOR_END__ = .;
   . = ALIGN(16);
   __DTOR_LIST__ = .;
   *(SORT(.dtors.*))
   *(.dtors)
   __DTOR_END__ = .;
   . = ALIGN(16);
   __fini_array_start = .;
   *(SORT(.fini_array.*))
   *(.fini_array)
   __fini_array_end = .;

   /* BSS merged into .data (EFI loader dislikes a separate .bss section) */
   . = ALIGN(16);
   _bss = .;
   *(.sbss)
   *(.scommon)
   *(.dynbss)
   *(.bss*)
   *(COMMON)
   *(.rel.local)
   . = ALIGN(16);
   _bss_end = .;
   _evdata = .;
   . = ALIGN(4096);
  }
  _edata = .;
  _data_vsize = _evdata - _data;
  _data_size = . - _data;

  . = ALIGN(4096);
  .vendor_cert : { *(.vendor_cert) }

  . = ALIGN(4096);
  .rela :
  {
    *(.rela.text*)
    *(.rela.data*)
    *(.rela.got)
    *(.rela.dyn)
    *(.rela.stab)
    *(.rela.init_array*)
    *(.rela.fini_array*)
    *(.rela.ctors*)
    *(.rela.dtors*)
  }
  . = ALIGN(4096);
  .rela.plt : { *(.rela.plt) }

  . = ALIGN(4096);
  .rodata :
  {
    _rodata = .;
    *(.rodata*)
    _evrodata = .;
    . = ALIGN(4096);
  }
  _erodata = .;
  _rodata_size = . - _rodata;
  _rodata_vsize = _evrodata - _rodata;
  _alldata_size = _erodata - _data;

  . = ALIGN(4096);
  .sbat :
  {
    _sbat = .;
    *(.sbat)
    *(.sbat.*)
    _esbat = .;
    . = ALIGN(4096);
    _epsbat = .;
  }
  _sbat_size = _epsbat - _sbat;
  _sbat_vsize = _esbat - _sbat;

  . = ALIGN(4096);
  .reloc :
  {
    _reloc = .;
    KEEP (*(.reloc))
    _evreloc = .;
    . = ALIGN(4096);
  }
  _ereloc = .;
  _reloc_size = . - _reloc;
  _reloc_vsize = _evreloc - _reloc;

  . = ALIGN(4096);
  .dynsym   : { *(.dynsym) }
  . = ALIGN(4096);
  .dynstr   : { *(.dynstr) }
  . = ALIGN(4096);
  .hash     : { *(.hash) }
  .gnu.hash : { *(.gnu.hash) }
  . = ALIGN(4096);
  .note.gnu.build-id : { *(.note.gnu.build-id) }

  _image_end = .;

  /DISCARD/ :
  {
    *(.rela.reloc)
    *(.eh_frame)
    *(.eh_frame_hdr)
    *(.gcc_except_table*)
    *(.note.GNU-stack)
  }
  .comment 0 : { *(.comment) }
}
