/* bgsize.r - SIZE -1 for tools that must keep running in the background under MultiFinder
 * (TcpBench and AfpCopy run side by side in hardware session 3). It replaces Retro68's default
 * (cannotBackground, 1 MB): canBackground, suspend/resume events, a 384 KB partition so two tools
 * fit beside the Finder on a 4 MB Classic. */
#include "Processes.r"

resource 'SIZE' (-1) {
    reserved,
    acceptSuspendResumeEvents,
    reserved,
    canBackground,
    needsActivateOnFGSwitch,
    backgroundAndForeground,
    dontGetFrontClicks,
    ignoreChildDiedEvents,
    is32BitCompatible,
    notHighLevelEventAware,
    onlyLocalHLEvents,
    notStationeryAware,
    dontUseTextEditServices,
    reserved,
    reserved,
    reserved,
    384 * 1024,
    384 * 1024
};
