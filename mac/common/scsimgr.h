/* scsimgr.h - the original SCSI Manager (_SCSIDispatch, 0xA815), declared from Inside
 * Macintosh: Devices because Multiversal has no SCSI.h. Selector word pushed last. */
#ifndef SCSIMGR_H
#define SCSIMGR_H

#include <MacTypes.h>

#pragma pack(push, 2)
typedef struct SCSIInstr { short scOpcode; long scParam1; long scParam2; } SCSIInstr;
#pragma pack(pop)
enum { scInc = 1, scNoInc = 2, scAdd = 3, scMove = 4, scLoop = 5, scNop = 6, scStop = 7, scComp = 8 };

/* SCSI Manager result codes (Inside Macintosh: Devices) */
enum { scCommErr = 2, scArbNBErr = 3, scBadParmsErr = 4, scPhaseErr = 5, scCompareErr = 6,
       scMgrBusyErr = 7, scSequenceErr = 8, scBusTOErr = 9, scComplPhaseErr = 10 };

/* SCSIStat: the 5380's Current SCSI Bus Status register, active high. */
enum { scsiStatBSY = 0x40, scsiStatREQ = 0x20, scsiStatMSG = 0x10, scsiStatCD = 0x08,
       scsiStatIO = 0x04 };

pascal OSErr SCSIReset(void) M68K_INLINE(0x4267, 0xA815);
pascal OSErr SCSIGet(void) M68K_INLINE(0x3F3C, 0x0001, 0xA815);
pascal OSErr SCSISelect(short targetID) M68K_INLINE(0x3F3C, 0x0002, 0xA815);
pascal OSErr SCSICmd(Ptr buffer, short count) M68K_INLINE(0x3F3C, 0x0003, 0xA815);
pascal OSErr SCSIComplete(short *stat, short *message, unsigned long wait)
    M68K_INLINE(0x3F3C, 0x0004, 0xA815);
pascal OSErr SCSIRead(Ptr tibPtr) M68K_INLINE(0x3F3C, 0x0005, 0xA815);
pascal OSErr SCSIWrite(Ptr tibPtr) M68K_INLINE(0x3F3C, 0x0006, 0xA815);
pascal OSErr SCSIRBlind(Ptr tibPtr) M68K_INLINE(0x3F3C, 0x0008, 0xA815);
pascal OSErr SCSIWBlind(Ptr tibPtr) M68K_INLINE(0x3F3C, 0x0009, 0xA815);
pascal short SCSIStat(void) M68K_INLINE(0x3F3C, 0x000A, 0xA815);

#endif
