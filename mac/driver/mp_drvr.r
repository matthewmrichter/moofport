/* mp_drvr.r - MoofPort's driver resource, shared by moofport.r (as 'DRVR' 128, which the INIT
 * installs) and by the installer (as 'MPDV' 128, which it copies into the System file as
 * 'DRVR' 47). Define MP_DRVR_TYPE and MP_DRVR_ATTRS before including; the defaults are
 * 'DRVR' and sysheap, locked.
 * Header layout: see moofport.r. Rez runs in the build directory (MoofPortDrv.flt). */
#ifndef MP_DRVR_TYPE
#define MP_DRVR_TYPE 'DRVR'
#endif
#ifndef MP_DRVR_ATTRS
#define MP_DRVR_ATTRS sysheap, locked
#endif

type MP_DRVR_TYPE {
    hex string;     /* header + name + stubs */
    hex string;     /* flat code */
};

resource MP_DRVR_TYPE (128, ".ENET0", MP_DRVR_ATTRS) {
    $"4C00 0000 0000 0000"          /* flags, delay, emask, menu */
    $"001A 001E 0022 0026 002A"     /* open, prime, control, status, close offsets */
    $"06 2E 45 4E 45 54 30 00"      /* "\p.ENET0" then 1 pad byte */
    $"7000 6014"                    /* 26 open */
    $"7001 6010"                    /* 30 prime */
    $"7002 600C"                    /* 34 control */
    $"7003 6008"                    /* 38 status */
    $"7004 6004"                    /* 42 close */
    $"7005 4E71",                   /* 46 boot */
    $$read("MoofPortDrv.flt")
};

