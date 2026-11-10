# mac/CMakeLists.txt - the Classic Mac side of MoofPort (Phase 0 tools).
#
# Built by scripts/build-mac.sh, which configures this with the Retro68 toolchain file and
# -m68000 (C and asm). Two recipes are used, both from MoofTCP (docs/dev/TOOLCHAIN.md):
#   - add_application(Name src... [Name.r])  for ordinary APPL tools. Retro68 builds Name
#     (68k code) and Name_APPL (the MacBinary Name.bin); build-mac.sh builds *_APPL. A .r
#     among the sources is compiled by Rez and bundled (here: ALRT/DITL 128 for the result
#     alert). The app type is APPL and the creator defaults to '????' (cosmetic; the tools
#     work regardless, and deploy.sh keeps whatever Rez set).
#   - the code-resource recipe (flat executable via -Wl,--mac-flat, then Rez) for SpyENET's
#     INIT + DRVR, exactly as mac/moof/moof.r in MoofTCP and mac/hello_init here.
#
# REZ / REZ_INCLUDE_PATH are provided by the Retro68 toolchain file.

cmake_minimum_required(VERSION 3.13)
project(moofport_mac C ASM)
include_directories(common)
# mac/icons/icons.r (scripts/icons.py) is included by the applications' .r files.
list(APPEND REZ_INCLUDE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/icons)
# REZ_INCLUDE_PATH is now a list: the custom Rez commands below take it as -I options.
set(REZ_INC ${REZ_INCLUDE_PATH})
list(TRANSFORM REZ_INC PREPEND -I)

# Icons: the Finder reads an application's BNDL only with hasBundle set; Retro68 writes flags 0.
function(moofport_bundle_bit target)
    add_custom_command(TARGET ${target}_APPL POST_BUILD
        COMMAND python3 -I ${CMAKE_CURRENT_SOURCE_DIR}/../scripts/mbflags.py ${target}.bin 0x2000)
endfunction()

# ---- ordinary applications ----

# HeapInfo and HeapInfoOpen: one source, two apps (OPEN_ENET opens ".ENET0" first).
set(RUNID common/runid.c common/runid.r)

add_application(HeapInfo      heapinfo/heapinfo.c heapinfo/heapinfo.r ${RUNID})
add_application(HeapInfoOpen  heapinfo/heapinfo.c heapinfo/heapinfo.r ${RUNID})
target_compile_definitions(HeapInfoOpen PRIVATE OPEN_ENET)

add_application(CpuMeter   cpumeter/cpumeter.c   cpumeter/cpumeter.r ${RUNID})

add_application(EnetBench  enetbench/enetbench.c enetbench/enetbench_ph.s enetbench/enetbench.r ${RUNID})

add_application(TcpBench   tcpbench/tcpbench.c   tcpbench/tcpbench.r ${RUNID} common/bgsize.r)

add_application(ScsiProbe  scsiprobe/scsiprobe.c scsiprobe/scsiprobe_tm.s scsiprobe/scsiprobe.r ${RUNID})

add_application(SpyDump    spyenet/spydump.c     spyenet/spydump.r ${RUNID})

add_application(MoofStat   moofstat/moofstat.c   moofstat/moofstat.r ${RUNID} CREATOR "MPST")
moofport_bundle_bit(MoofStat)
target_include_directories(MoofStat PRIVATE driver ${CMAKE_CURRENT_SOURCE_DIR}/../core)

# AfpCopy: AppleShare round trip (Phase 2), with MoofPort's counters.
add_application(AfpCopy    afpcopy/afpcopy.c     afpcopy/afpcopy.r ${RUNID} common/bgsize.r)
target_include_directories(AfpCopy PRIVATE driver ${CMAKE_CURRENT_SOURCE_DIR}/../core)

# MoofTune: poll timing, blind reads and checksum verification (Phase 3, private Control 'MT').
add_application(MoofTune   mooftune/mooftune.c   mooftune/mooftune.r ${RUNID} CREATOR "MPTU")
moofport_bundle_bit(MoofTune)
target_include_directories(MoofTune PRIVATE driver ${CMAKE_CURRENT_SOURCE_DIR}/../core)

# ---- SpyENET: INIT + spy DRVR (code-resource recipe) ----

# The installer, a flat Retro68 block -> resource INIT 128.
add_executable(SpyInit spyenet/spyenet_init.c)
set_target_properties(SpyInit PROPERTIES OUTPUT_NAME SpyInit.flt)
target_link_options(SpyInit PRIVATE -Wl,--mac-flat)

# The spy driver: entry dispatcher (defines _start), driver C, flat block -> resource DRVR 128.
add_executable(SpyDrv spyenet/spy_drv_stubs.s spyenet/spy_drv.c)
set_target_properties(SpyDrv PROPERTIES OUTPUT_NAME SpyDrv.flt LINKER_LANGUAGE C)
target_link_options(SpyDrv PRIVATE -Wl,--mac-flat)

add_custom_command(
    OUTPUT SpyENET.bin SpyENET.dsk
    COMMAND ${REZ} ${REZ_INC}
            ${CMAKE_CURRENT_SOURCE_DIR}/spyenet/spyenet.r
            -o SpyENET.bin --cc SpyENET.dsk
            -t INIT -c MPSY
    DEPENDS SpyInit SpyDrv spyenet/spyenet.r)
# Named *_APPL so scripts/build-mac.sh builds the MacBinary for `SpyENET`.
add_custom_target(SpyENET_APPL ALL DEPENDS SpyENET.bin)

# ---- MoofPort: the driver (Phase 1) ----

set(MP_CORE ${CMAKE_CURRENT_SOURCE_DIR}/../core)
add_executable(MoofPortInit driver/mp_init.c)
set_target_properties(MoofPortInit PROPERTIES OUTPUT_NAME MoofPortInit.flt)
target_compile_options(MoofPortInit PRIVATE -Os -Wall -Wextra -Werror)
target_link_options(MoofPortInit PRIVATE -Wl,--mac-flat)

add_executable(MoofPortDrv driver/mp_drv_stubs.s driver/mp_drv.c driver/mp_drv_ph.s driver/mp_scsi.c
    ${MP_CORE}/mp_ring.c ${MP_CORE}/mp_dev.c ${MP_CORE}/mp_dev_rx.c ${MP_CORE}/mp_dev_tx.c
    ${MP_CORE}/mp_demux.c ${MP_CORE}/mp_poll.c ${MP_CORE}/mp_budget.c ${MP_CORE}/mp_ipck.c)
set_target_properties(MoofPortDrv PROPERTIES OUTPUT_NAME MoofPortDrv.flt LINKER_LANGUAGE C)
target_include_directories(MoofPortDrv PRIVATE ${MP_CORE} driver)
target_compile_options(MoofPortDrv PRIVATE $<$<COMPILE_LANGUAGE:C>:-Os -mpcrel -std=c99 -Wall -Wextra -Werror -ffunction-sections -fdata-sections>)
target_link_options(MoofPortDrv PRIVATE -Wl,--mac-flat -Wl,--gc-sections)
# No MacsBug function names in the resident driver: about 1 KB.
set_target_properties(MoofPortDrv PROPERTIES C_COMPILER_LAUNCHER ${CMAKE_CURRENT_SOURCE_DIR}/tools/nomacsbug.sh)

add_custom_command(
    OUTPUT MoofPort.bin MoofPort.dsk
    COMMAND ${REZ} ${REZ_INC} -I${CMAKE_CURRENT_SOURCE_DIR}/driver
            ${CMAKE_CURRENT_SOURCE_DIR}/driver/moofport.r
            -o MoofPort.bin --cc MoofPort.dsk
            -t INIT -c MPRT
    DEPENDS MoofPortInit MoofPortDrv driver/moofport.r driver/mp_drvr.r)
add_custom_target(MoofPort_APPL ALL DEPENDS MoofPort.bin)

# MoofPortInstall: puts the driver into the System file as DRVR 47 (Phase 2, EtherTalk). Its
# resources are Rezzed here, in the build directory, so mp_drvr.r finds MoofPortDrv.flt.
add_custom_command(
    OUTPUT InstallerRes.rsrc.bin
    COMMAND ${REZ} ${REZ_INC} -I${CMAKE_CURRENT_SOURCE_DIR}/driver
            -I${CMAKE_CURRENT_SOURCE_DIR}/icons
            ${CMAKE_CURRENT_SOURCE_DIR}/installer/installer.r -o InstallerRes.rsrc.bin
    DEPENDS MoofPortDrv installer/installer.r driver/mp_drvr.r icons/icons.r)
add_application(MoofPortInstall installer/installer.c ${RUNID}
                ${CMAKE_CURRENT_BINARY_DIR}/InstallerRes.rsrc.bin CREATOR "MPIN")
moofport_bundle_bit(MoofPortInstall)

# ---- hwsend diagnostic kit (docs/dev/hwsend.md): never part of a release ----

# The driver with the timing hooks (mp_diag.c, -DMP_DIAG). Without MP_DIAG, mp_drv.c is unchanged.
add_executable(MoofPortDiagDrv driver/mp_drv_stubs.s driver/mp_drv.c driver/mp_drv_ph.s driver/mp_scsi.c
    driver/mp_diag.c driver/mp_diag_s.s
    ${MP_CORE}/mp_ring.c ${MP_CORE}/mp_dev.c ${MP_CORE}/mp_dev_rx.c ${MP_CORE}/mp_dev_tx.c
    ${MP_CORE}/mp_demux.c ${MP_CORE}/mp_poll.c ${MP_CORE}/mp_budget.c ${MP_CORE}/mp_ipck.c)
set_target_properties(MoofPortDiagDrv PROPERTIES OUTPUT_NAME MoofPortDiagDrv.flt LINKER_LANGUAGE C)
target_include_directories(MoofPortDiagDrv PRIVATE ${MP_CORE} driver)
target_compile_definitions(MoofPortDiagDrv PRIVATE MP_DIAG)
target_compile_options(MoofPortDiagDrv PRIVATE $<$<COMPILE_LANGUAGE:C>:-Os -mpcrel -std=c99 -Wall -Wextra -Werror -ffunction-sections -fdata-sections>)
target_link_options(MoofPortDiagDrv PRIVATE -Wl,--mac-flat -Wl,--gc-sections)

# MoofPortDiag: MoofPortInstall carrying the diagnostic driver. mp_drvr.r reads "MoofPortDrv.flt"
# from Rez's directory, so Rez runs in diagres/, where that name is a copy of the diagnostic driver.
add_custom_command(
    OUTPUT diagres/InstallerRes.rsrc.bin
    COMMAND ${CMAKE_COMMAND} -E make_directory diagres
    COMMAND ${CMAKE_COMMAND} -E copy MoofPortDiagDrv.flt diagres/MoofPortDrv.flt
    COMMAND ${CMAKE_COMMAND} -E chdir diagres ${REZ} ${REZ_INC} -I${CMAKE_CURRENT_SOURCE_DIR}/driver
            -I${CMAKE_CURRENT_SOURCE_DIR}/icons
            ${CMAKE_CURRENT_SOURCE_DIR}/installer/installer.r -o InstallerRes.rsrc.bin
    DEPENDS MoofPortDiagDrv installer/installer.r driver/mp_drvr.r icons/icons.r)
add_application(MoofPortDiag installer/installer.c ${RUNID}
                ${CMAKE_CURRENT_BINARY_DIR}/diagres/InstallerRes.rsrc.bin CREATOR "MPDG")
target_compile_definitions(MoofPortDiag PRIVATE MP_DIAG)

# TcpSend: TcpBench's send and echo, in three send modes, with per-chunk timing and the
# diagnostic driver's counters per mode.
add_application(TcpSend    tcpsend/tcpsend.c tcpsend/tcpsend_tm.s tcpsend/tcpsend.r ${RUNID} common/bgsize.r CREATOR "MPTS")
target_include_directories(TcpSend PRIVATE driver ${CMAKE_CURRENT_SOURCE_DIR}/../core)

# MoofBatch (hwsend11): TcpSend's modes for every MoofTune setting in one run, with UDP markers
# for the PC capture (mac/tcpsend/tcpsend.c, MP_BATCH).
add_application(MoofBatch  tcpsend/tcpsend.c tcpsend/tcpsend_tm.s tcpsend/tcpsend.r ${RUNID} common/bgsize.r CREATOR "MPMB")
target_include_directories(MoofBatch PRIVATE driver ${CMAKE_CURRENT_SOURCE_DIR}/../core)
target_compile_definitions(MoofBatch PRIVATE MP_BATCH)
