/* installer.c - MoofPortInstall: put MoofPort into the System file as the EtherTalk/MacTCP
 * ".ENET0" driver, or take it out again. The Mac-side counterpart of scripts/sysdrvr.py.
 *
 * Why the System file (PLAN decision 3, docs/dev/BOOT-CRASH-ETHERTALK.md): with EtherTalk
 * selected, AppleTalk opens ".ENET0" from the System file before any extension INIT runs, so the
 * INIT version of MoofPort comes too late. As DRVR 47 MoofPort is the driver AppleTalk opens.
 *
 * Install:   DaynaPORT's DRVR 47 ".ENET0" is kept, unchanged, as type 'DRVx' 47 (same id, name,
 *            attributes and bytes); MoofPort's driver (resource 'MPDV' 128 of this app) is added
 *            as DRVR 47 ".ENET0", sysheap + locked. An INIT-version MoofPort in the System Folder
 *            (type INIT, creator MPRT) gets file type 'INIx', so it no longer loads and the two
 *            never both run.
 * Fresh:     a System file with no DRVR 47 and no DRVx 47 (DaynaPORT never installed) gets
 *            MoofPort's driver as DRVR 47 the same way, with nothing to keep. EtherTalk then needs
 *            only Apple's AppleTalk (docs/dev/INSTALLER.md).
 * Uninstall: our DRVR 47 is removed and DRVx 47 becomes DRVR 47 again; after a fresh install
 *            there is no DRVx 47, so the System file is left without an .ENET0. A disabled INIT
 *            stays disabled.
 * Update:    when MoofPort is installed, the alert offers Update, Uninstall or Cancel. Update
 *            replaces our DRVR 47 with this installer's driver; DaynaPORT's DRVx 47 is not
 *            touched.
 * Both run only after the person clicks Install or Uninstall in the confirm alert, and take
 * effect at the next restart. They refuse when the System file is not in a state they know (a
 * DRVx 47 without DRVR 47, a DRVx 47 beside a DRVR 47 that is not MoofPort, a DRVR 47 that is
 * not .ENET0) or the disk is short of space. A
 * driver that is open right now (AppleTalk or MacTCP) is removed from the resource map only, not
 * from memory, so it keeps working until the restart.
 *
 * Output: Installer-<run id>.out next to the app (runid.c).
 */
#include <Quickdraw.h>
#include <Fonts.h>
#include <Windows.h>
#include <Menus.h>
#include <TextEdit.h>
#include <Dialogs.h>
#include <Memory.h>
#include <Files.h>
#include <Resources.h>
#include <stdio.h>
#include <string.h>
#include "runid.h"

QDGlobals qd;

#define ENET_ID    47
/* MoofPortDiag (-DMP_DIAG, the hwsend kits) is this installer carrying the diagnostic driver.
 * "send-fix": branch send-fix's driver (queued EWrites, bounded poll ticks, blind frame writes);
 * the release thread sets the real version when it takes the change. */
#ifdef MP_DIAG
#define MP_INST_VER "1.0 diagnostic"
#else
#define MP_INST_VER "1.0"
#endif
#define T_DRVR     'DRVR'
#define T_PARKED   'DRVx'           /* DaynaPORT's driver while MoofPort is installed */
#define T_OURS     'MPDV'           /* MoofPort's driver inside this app */
#define T_INIT     'INIT'
#define T_INIT_OFF 'INIx'
#define C_MOOFPORT 'MPRT'
#define ATTR_DRVR  (resSysHeap | resLocked)
#define SLACK      (32L * 1024)     /* free space wanted beyond the new resources */

enum { kResult = 128, kAskInstall = 129, kAskUninstall = 130 };

static short g_outref = -1;
static Str255 g_outname;
static char g_line[400];
static void wr(const char *s) { long n = (long)strlen(s); if (g_outref >= 0) FSWrite(g_outref, &n, (Ptr)s); }
#define LOG(...) do { sprintf(g_line, __VA_ARGS__); wr(g_line); } while (0)

static char g_msg[240];
static void say(const char *m) { strncpy(g_msg, m, sizeof g_msg - 1); LOG("%s\r", m); }

static void ctop(const char *c, Str255 p)
{
    size_t n = strlen(c);
    if (n > 255) n = 255;
    p[0] = (unsigned char)n;
    memcpy(p + 1, c, n);
}

static void type_str(ResType t, char *s)
{
    s[0] = (char)(t >> 24); s[1] = (char)(t >> 16); s[2] = (char)(t >> 8); s[3] = (char)t; s[4] = 0;
}

/* ---- the System file ---- */

static short g_sys;          /* the System file's resource refnum */
static short g_sys_vref;     /* its volume */
static long  g_sys_dir;      /* the System Folder's dirID */

static int find_system(void)
{
    FCBPBRec pb;
    Str255 name;
    g_sys = LMGetSysMap();
    memset(&pb, 0, sizeof pb);
    pb.ioNamePtr = name;
    pb.ioRefNum = g_sys;
    if (PBGetFCBInfoSync(&pb) != noErr) return -1;
    g_sys_vref = pb.ioFCBVRefNum;
    g_sys_dir = pb.ioFCBParID;
    LOG("System file \"%.*s\" refnum %d on vRefNum %d, System Folder dirID %ld\r", (int)name[0],
        (const char *)name + 1, (int)g_sys, (int)g_sys_vref, g_sys_dir);
    return 0;
}

static long free_bytes(void)
{
    HParamBlockRec pb;
    memset(&pb, 0, sizeof pb);
    pb.volumeParam.ioVRefNum = g_sys_vref;
    if (PBHGetVInfoSync(&pb) != noErr) return -1;
    return (long)pb.volumeParam.ioVFrBlk * pb.volumeParam.ioVAlBlkSiz;
}

/* A resource of the System file only (Get1Resource with the System file current). */
static Handle sys_res(ResType t, short id)
{
    short cur = CurResFile();
    Handle h;
    UseResFile(g_sys);
    h = Get1Resource(t, id);
    UseResFile(cur);
    return h;
}

static void describe(const char *what, Handle h)
{
    short id;
    ResType t;
    Str255 name;
    char ts[5];
    if (!h) { LOG("%s: none\r", what); return; }
    GetResInfo(h, &id, &t, name);
    type_str(t, ts);
    LOG("%s: '%s' %d \"%.*s\" attrs 0x%02x, %ld bytes\r", what, ts, (int)id, (int)name[0],
        (const char *)name + 1, (unsigned)GetResAttrs(h) & 0xFF, (long)GetHandleSize(h));
}

/* Is `h` a MoofPort driver? Compares the first 50 bytes (DRVR header, name and entry stubs, see
 * mac/driver/moofport.r), which never change. The rest does: a loaded, opened driver keeps its
 * globals in its own block, so a running MoofPort differs from this installer's copy. */
#define MP_HDR_LEN 50
static int is_moofport(Handle h, Handle ours)
{
    return GetHandleSize(h) > MP_HDR_LEN && memcmp(*h, *ours, MP_HDR_LEN) == 0;
}

/* Add a copy of `src` to the System file as (t, id, name, attrs). The copy lives in the system
 * heap: the System file's map outlives this application's heap. Returns the new handle or 0. */
static Handle add_copy(Handle src, ResType t, short id, ConstStr255Param name, short attrs)
{
    long n = GetHandleSize(src);
    Handle h = NewHandleSys(n);
    short cur = CurResFile();
    if (!h) { LOG("NewHandleSys(%ld) failed, %d\r", n, (int)MemError()); return 0; }
    BlockMove(*src, *h, n);
    UseResFile(g_sys);
    AddResource(h, t, id, name);
    if (ResError() != noErr) {
        LOG("AddResource failed, %d\r", (int)ResError());
        UseResFile(cur);
        DisposeHandle(h);
        return 0;
    }
    SetResAttrs(h, attrs);
    ChangedResource(h);
    UseResFile(cur);
    return h;
}

/* Remove `h` from the System file's map. `in_use`: the driver may be open now, so its memory
 * stays (the DCE points at it); otherwise the handle is disposed. */
static int remove_res(Handle h, int in_use)
{
    short cur = CurResFile();
    UseResFile(g_sys);
    SetResAttrs(h, GetResAttrs(h) & ~resProtected);
    RemoveResource(h);
    UseResFile(cur);
    if (ResError() != noErr) { LOG("RemoveResource failed, %d\r", (int)ResError()); return -1; }
    if (!in_use) DisposeHandle(h);
    return 0;
}

static int write_system(void)
{
    UpdateResFile(g_sys);
    if (ResError() != noErr) { LOG("UpdateResFile failed, %d\r", (int)ResError()); return -1; }
    FlushVol(0, g_sys_vref);
    return 0;
}

/* ---- the INIT version ---- */

/* Find INIT files of creator MPRT in the System Folder; with `disable`, retype them 'INIx'.
 * Returns how many enabled ones there are (before disabling). */
static int init_versions(int disable)
{
    CInfoPBRec pb;
    Str255 name;
    short i;
    int n = 0;
    for (i = 1; ; i++) {
        memset(&pb, 0, sizeof pb);
        name[0] = 0;
        pb.hFileInfo.ioNamePtr = name;
        pb.hFileInfo.ioVRefNum = g_sys_vref;
        pb.hFileInfo.ioDirID = g_sys_dir;
        pb.hFileInfo.ioFDirIndex = i;
        if (PBGetCatInfoSync(&pb) != noErr) break;
        if (pb.hFileInfo.ioFlAttrib & 0x10) continue;            /* a folder */
        if (pb.hFileInfo.ioFlFndrInfo.fdCreator != C_MOOFPORT) continue;
        if (pb.hFileInfo.ioFlFndrInfo.fdType != T_INIT) continue;
        n++;
        LOG("INIT-version MoofPort in the System Folder: \"%.*s\"\r", (int)name[0], (const char *)name + 1);
        if (!disable) continue;
        pb.hFileInfo.ioFlFndrInfo.fdType = T_INIT_OFF;
        pb.hFileInfo.ioDirID = g_sys_dir;
        if (PBSetCatInfoSync(&pb) == noErr) LOG("  file type set to 'INIx': it no longer loads\r");
        else LOG("  could not change its file type\r");
    }
    return n;
}

/* ---- install / uninstall ---- */

/* Show alert `alrt` with `text`; returns the item hit (1 = the first button). */
static short ask_item(short alrt, const char *text)
{
    Str255 p;
    ctop(text, p);
    ParamText(p, (ConstStr255Param)"\p", (ConstStr255Param)"\p", (ConstStr255Param)"\p");
    return Alert(alrt, 0);
}

static int ask(short alrt, const char *text) { return ask_item(alrt, text) == 1; }

/* No DRVR 47 or DRVx 47: add MoofPort's driver, nothing to keep. */
static void install_fresh(Handle ours)
{
    long need = GetHandleSize(ours) + SLACK, have = free_bytes();
    Handle mine;
    int inits = init_versions(0);
    char q[240];

    LOG("free space %ld bytes, need %ld\r", have, need);
    if (have < need) { say("Not enough free space on the startup disk. Nothing was changed."); return; }
    sprintf(q, "No DaynaPORT driver is installed. Install MoofPort as the Ethernet driver (.ENET0) "
               "in the System file?%s Restart afterwards.",
            inits ? " The MoofPort extension in the System Folder will be turned off." : "");
    if (!ask(kAskInstall, q)) { say("Cancelled. Nothing was changed."); return; }
    if (!(mine = add_copy(ours, T_DRVR, ENET_ID, (ConstStr255Param)"\p.ENET0", ATTR_DRVR))) {
        say("Could not add MoofPort's driver. Nothing was changed.");
        return;
    }
    if (write_system()) { say("Writing the System file failed. Restore your backup before restarting."); return; }
    ReleaseResource(mine);
    if (inits) init_versions(1);
    describe("now DRVR 47", sys_res(T_DRVR, ENET_ID));
    say("MoofPort is installed. Restart the Mac to use it.");
}

static void install(Handle dayna, Handle ours)
{
    Str255 name;
    short id;
    ResType t;
    short attrs = GetResAttrs(dayna);
    long need = GetHandleSize(dayna) + GetHandleSize(ours) + SLACK, have = free_bytes();
    Handle parked, mine;
    int inits = init_versions(0);
    char q[240];

    LOG("free space %ld bytes, need %ld\r", have, need);
    if (have < need) { say("Not enough free space on the startup disk. Nothing was changed."); return; }
    sprintf(q, "Install MoofPort as the Ethernet driver (.ENET0) in the System file? DaynaPORT's "
               "driver is kept for Uninstall.%s Restart afterwards.",
            inits ? " The MoofPort extension in the System Folder will be turned off." : "");
    if (!ask(kAskInstall, q)) { say("Cancelled. Nothing was changed."); return; }

    GetResInfo(dayna, &id, &t, name);
    if (!(parked = add_copy(dayna, T_PARKED, ENET_ID, name, attrs))) {
        say("Could not keep DaynaPORT's driver. Nothing was changed.");
        return;
    }
    if (remove_res(dayna, 1)) {
        remove_res(parked, 0);
        say("Could not take out DaynaPORT's driver. Nothing was changed.");
        return;
    }
    if (!(mine = add_copy(ours, T_DRVR, ENET_ID, (ConstStr255Param)"\p.ENET0", ATTR_DRVR))) {
        /* put DaynaPORT back the way it was */
        add_copy(parked, T_DRVR, ENET_ID, name, attrs);
        remove_res(parked, 0);
        write_system();
        say("Could not add MoofPort's driver. DaynaPORT's driver was put back.");
        return;
    }
    if (write_system()) { say("Writing the System file failed. Restore your backup before restarting."); return; }
    ReleaseResource(mine);
    ReleaseResource(parked);
    if (inits) init_versions(1);
    describe("now DRVR 47", sys_res(T_DRVR, ENET_ID));
    describe("now DRVx 47", sys_res(T_PARKED, ENET_ID));
    say("MoofPort is installed. Restart the Mac to use it.");
}

/* Replace our DRVR 47 with this installer's driver. */
static void update(Handle mine, Handle ours)
{
    Handle h;
    if (remove_res(mine, 1)) { say("Could not take out the installed MoofPort. Nothing was changed."); return; }
    if (!(h = add_copy(ours, T_DRVR, ENET_ID, (ConstStr255Param)"\p.ENET0", ATTR_DRVR))) {
        add_copy(mine, T_DRVR, ENET_ID, (ConstStr255Param)"\p.ENET0", ATTR_DRVR);
        write_system();
        say("Could not add the new driver. The installed MoofPort stays.");
        return;
    }
    if (write_system()) { say("Writing the System file failed. Restore your backup before restarting."); return; }
    ReleaseResource(h);
    describe("now DRVR 47", sys_res(T_DRVR, ENET_ID));
    say("MoofPort is updated. Restart the Mac to use the new version.");
}

/* MoofPort is DRVR 47; `parked` is DaynaPORT's DRVx 47, or 0 after a fresh install. */
static void uninstall(Handle mine, Handle parked, Handle ours)
{
    Str255 name;
    short id;
    ResType t;
    short attrs, hit;
    long need = (parked ? GetHandleSize(parked) : 0) + GetHandleSize(ours) + SLACK, have = free_bytes();
    Handle dayna;

    LOG("free space %ld bytes, need %ld\r", have, need);
    if (have < need) { say("Not enough free space on the startup disk. Nothing was changed."); return; }
    hit = ask_item(kAskUninstall, parked
        ? "MoofPort is installed. Update it to this version (" MP_INST_VER "), or uninstall it and put "
          "DaynaPORT's Ethernet driver back? Restart afterwards."
        : "MoofPort is installed (no DaynaPORT driver was kept). Update it to this version "
          "(" MP_INST_VER "), or uninstall it? The Mac then has no Ethernet driver. Restart afterwards.");
    if (hit == 1) { update(mine, ours); return; }
    if (hit != 3) {                 /* item 2 is Cancel, the default (installer.r) */
        say("Cancelled. Nothing was changed.");
        return;
    }
    if (!parked) {
        if (remove_res(mine, 1)) { say("Could not take out MoofPort's driver. Nothing was changed."); return; }
        if (write_system()) { say("Writing the System file failed. Restore your backup before restarting."); return; }
        describe("now DRVR 47", sys_res(T_DRVR, ENET_ID));
        say("MoofPort is uninstalled. The Mac has no Ethernet driver now. Restart the Mac.");
        return;
    }
    attrs = GetResAttrs(parked);
    GetResInfo(parked, &id, &t, name);
    if (remove_res(mine, 1)) { say("Could not take out MoofPort's driver. Nothing was changed."); return; }
    if (!(dayna = add_copy(parked, T_DRVR, ENET_ID, name, attrs))) {
        add_copy(mine, T_DRVR, ENET_ID, (ConstStr255Param)"\p.ENET0", ATTR_DRVR);
        write_system();
        say("Could not put DaynaPORT's driver back. MoofPort stays installed.");
        return;
    }
    if (remove_res(parked, 0)) { say("Could not remove the kept copy. Restore your backup before restarting."); return; }
    if (write_system()) { say("Writing the System file failed. Restore your backup before restarting."); return; }
    ReleaseResource(dayna);
    describe("now DRVR 47", sys_res(T_DRVR, ENET_ID));
    describe("now DRVx 47", sys_res(T_PARKED, ENET_ID));
    say("DaynaPORT's driver is back. Restart the Mac to use it.");
}

static void run(void)
{
    Handle ours, drvr, parked;
    Str255 name;
    short id;
    ResType t;

    if (find_system()) { say("Could not find the System file."); return; }
    ours = Get1Resource(T_OURS, 128);            /* this app's own file is current */
    if (!ours) { say("This copy of the installer is damaged (no MoofPort driver inside)."); return; }
    describe("MoofPort driver in this installer", ours);
    drvr = sys_res(T_DRVR, ENET_ID);
    parked = sys_res(T_PARKED, ENET_ID);
    describe("System file DRVR 47", drvr);
    describe("System file DRVx 47", parked);
    if (!drvr) {
        if (parked) say("The System file has a DRVx 47 but no DRVR 47. Nothing was changed.");
        else install_fresh(ours);
        return;
    }
    GetResInfo(drvr, &id, &t, name);
    if (!(name[0] == 6 && !memcmp(name + 1, ".ENET0", 6))) {
        say("The System file's DRVR 47 is not .ENET0. Nothing was changed.");
        return;
    }
    if (parked) {
        if (!is_moofport(drvr, ours)) {
            say("DRVx 47 exists but DRVR 47 is not MoofPort. Nothing was changed.");
            return;
        }
        uninstall(drvr, parked, ours);
    } else if (is_moofport(drvr, ours)) {
        uninstall(drvr, 0, ours);   /* a fresh install: Update or Uninstall, nothing to put back */
    } else {
        install(drvr, ours);
    }
}

int main(void)
{
    Str255 ps;

    MaxApplZone();
    InitGraf(&qd.thePort); InitFonts(); InitWindows(); InitMenus(); TEInit();
    InitDialogs(0); InitCursor();
    runid_name("Installer", ".out", g_outname);
    FSDelete(g_outname, 0);
    Create(g_outname, 0, 'ttxt', 'TEXT');
    if (FSOpen(g_outname, 0, &g_outref)) g_outref = -1;
    else SetEOF(g_outref, 0);
    wr("MoofPortInstall " MP_INST_VER "\r");

    run();

    if (g_outref >= 0) { FSClose(g_outref); g_outref = -1; }
    FlushVol(0, 0);
    ctop(g_msg, ps);
    ParamText(ps, (ConstStr255Param)"\p", (ConstStr255Param)"\p", (ConstStr255Param)"\p");
    Alert(kResult, 0);
    return 0;
}
