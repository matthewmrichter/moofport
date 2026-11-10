/* runid.c - the "name this run" dialog shared by the Phase 0 tools.
 *
 * The tools write their results next to themselves, and every run used to replace the last
 * one's file. On launch each tool now asks for a run id (DLOG 200, runid.r) and writes
 * "<Tool>-<id>.out". The id is remembered in the text file "MoofPort Run" in the same folder and
 * offered next time. If that file's first line starts with '!', the rest of the line is used as
 * the id without asking: that keeps unattended runs (Snow) unattended. Cancel or an empty id
 * gives the old "<Tool>.out". ':' (the HFS path separator) and control characters become '-',
 * and the id is cut so the whole name fits HFS's 31 characters.
 */
#include <Dialogs.h>
#include <Files.h>
#include <string.h>
#include "runid.h"

#define RUN_FILE "\pMoofPort Run"
enum { kDlog = 200, kOK = 1, kCancel = 2, kEdit = 4 };

static void read_last(unsigned char *id, int *quiet)
{
    short r;
    long len = 63;
    char buf[64];
    int i, n = 0;
    id[0] = 0;
    *quiet = 0;
    if (FSOpen((ConstStr255Param)RUN_FILE, 0, &r) != noErr) return;
    if (FSRead(r, &len, (Ptr)buf) != noErr && len <= 0) len = 0;
    FSClose(r);
    i = 0;
    if (len > 0 && buf[0] == '!') { *quiet = 1; i = 1; }
    for (; i < len && buf[i] != '\r' && buf[i] != '\n'; i++) id[++n] = (unsigned char)buf[i];
    id[0] = (unsigned char)n;
}

static void save_last(const unsigned char *id)
{
    short r;
    long len = id[0];
    Create((ConstStr255Param)RUN_FILE, 0, 'ttxt', 'TEXT');
    if (FSOpen((ConstStr255Param)RUN_FILE, 0, &r) != noErr) return;
    SetEOF(r, 0);
    FSWrite(r, &len, (Ptr)(id + 1));
    FSClose(r);
}

static void ask(unsigned char *id)
{
    DialogPtr d = GetNewDialog(kDlog, 0, (WindowPtr)-1);
    short item = 0, type;
    Handle h;
    Rect box;
    if (!d) return;                       /* no dialog resource: keep the remembered id */
    GetDialogItem(d, kEdit, &type, &h, &box);
    SetDialogItemText(h, id);
    SelectDialogItemText(d, kEdit, 0, 32767);
    while (item != kOK && item != kCancel) ModalDialog(0, &item);
    if (item == kOK) GetDialogItemText(h, id);
    else id[0] = 0;
    DisposeDialog(d);
}

unsigned char *runid_name(const char *tool, const char *ext, unsigned char *out)
{
    unsigned char id[256];
    int quiet, i, n = 0, room;
    size_t tl = strlen(tool), el = strlen(ext);

    read_last(id, &quiet);
    if (!quiet) {
        ask(id);
        if (id[0]) save_last(id);
    }
    for (i = 1; i <= id[0]; i++)          /* trim leading and trailing spaces */
        if (id[i] != ' ') break;
    while (id[0] >= i && id[id[0]] == ' ') id[0]--;

    memcpy(out + 1, tool, tl);
    n = (int)tl;
    room = 31 - (int)tl - (int)el - 1;
    if (id[0] >= i && room > 0) {
        out[++n] = '-';
        for (; i <= id[0] && room > 0; i++, room--) {
            unsigned char c = id[i];
            out[++n] = (c == ':' || c < 0x20 || c == 0x7F) ? '-' : c;
        }
    }
    memcpy(out + 1 + n, ext, el);
    out[0] = (unsigned char)(n + el);
    return out;
}
