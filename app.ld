/* SMS for fun overlay app: linked into the 4 KiB RAM overlay.
 * Code, constants and .data go into the .app file; .bss is NOLOAD, so it uses
 * overlay RAM after the code without making the file bigger (the loader zeroes the
 * whole 4 KiB). The rest of the overlay, up to 4 KiB, holds the message history. */
APP_VMA = DEFINED(APP_VMA) ? APP_VMA : 0x20000280;
APP_LENGTH = 0x1000;

MEMORY { APP (rwx) : ORIGIN = APP_VMA, LENGTH = APP_LENGTH }

ENTRY(app_main)

SECTIONS
{
  .app : {
    KEEP(*(.text.entry))
    *(.text .text.*)
    *(.rodata .rodata.*)
    . = ALIGN(4);
    *(.data .data.*)
    . = ALIGN(4);
  } > APP
  __app_code_end = .;

  .bss (NOLOAD) : {
    __app_bss_start = .;
    *(.bss .bss.* COMMON)
    . = ALIGN(4);
    __app_bss_end = .;
  } > APP
  __app_end = .;

  /* Historique des messages : toute la RAM d'overlay restante */
  __hist_start = __app_end;
  __hist_end   = APP_VMA + APP_LENGTH;

  ASSERT(__app_end <= APP_VMA + APP_LENGTH, "SMS for fun: overlay RAM overflow (4 KiB)")

  /DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) *(.eh_frame) *(.comment) *(.note.*) }
}
