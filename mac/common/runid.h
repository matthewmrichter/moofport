/* runid.h - name each run of a Phase 0 tool, so its output file is "<Tool>-<id>.out". */
#ifndef RUNID_H
#define RUNID_H

/* Ask for a run id (or take it from the "MoofPort Run" file, see runid.c) and build the
 * output file name: "<tool>-<id><ext>", or "<tool><ext>" for an empty id or Cancel. `out` is a
 * Pascal string of at most 31 characters (HFS). Call after InitDialogs. Returns `out`. */
unsigned char *runid_name(const char *tool, const char *ext, unsigned char *out);

#endif
