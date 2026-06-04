/*
//###########################################################################
//
// FILE:	F2800157_flash_lnk.cmd
//
// TITLE:	Linker Command File For F2800157 Device
//
//###########################################################################
*/
MEMORY
{
    RAMM0        : origin = 0x000128, length = 0x0002D8
    RAMM1        : origin = 0x000400, length = 0x0003F8
    RAMLS_VAR    : origin = 0x008000, length = 0x003000
    RAMLS_STK    : origin = 0x00B000, length = 0x001000

    BEGIN        : origin = 0x080000, length = 0x000002
    BOOT_FLASH   : origin = 0x080002, length = 0x001BFE

    RESET        : origin = 0x3FFFC0, length = 0x000002
}

SECTIONS
{
    .reset       : > RESET, TYPE = DSECT
    codestart    : > BEGIN

    .text        : > BOOT_FLASH, ALIGN(8)
    .const       : > BOOT_FLASH, ALIGN(8)
    .cinit       : > BOOT_FLASH, ALIGN(8)
    .init_array  : > BOOT_FLASH, ALIGN(8)
    .switch      : > BOOT_FLASH, ALIGN(8)

    .TI.ramfunc  :
        LOAD = BOOT_FLASH,
        RUN  = RAMLS_VAR,
        LOAD_START(RamfuncsLoadStart),
        LOAD_SIZE(RamfuncsLoadSize),
        RUN_START(RamfuncsRunStart),
        ALIGN(8)

    .stack       : > RAMLS_STK
    .bss         : > RAMLS_VAR
    .data        : > RAMLS_VAR
    .sysmem      : > RAMLS_STK
    .ebss        : > RAMLS_VAR
    .econst      : > BOOT_FLASH
}
/*
//###########################################################################
// End of file.
//###########################################################################
*/
