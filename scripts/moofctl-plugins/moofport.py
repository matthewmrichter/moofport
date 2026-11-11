"""moofport.py - the MoofPort plugin for moofctl (MoofAgent repo: docs/REMOTE-HARNESS.md "moofctl plugins").

    moofctl --plugins scripts/moofctl-plugins moofport switch dayna|moofport [--installer NAME] [--no-verify]
    moofctl --plugins scripts/moofctl-plugins moofport settings [--host IP] [--modes EACRAC] [--settings "new steady"] [--rbuf 16384]

switch: changes the Mac's .ENET0 driver (DaynaPORT <-> MoofPort) with no one at the Mac, by running MoofPort's own
installer unattended through MoofAgent (mac/installer/installer.c, "Unattended"):
  1. delete the old <installer>-sw-<target>.out and REQUIRE that it is gone;
  2. PUT MoofPortInstall.cfg: "install" (moofport) or "uninstall" (dayna);
  3. RUN the installer (run id sw-<target>), GET its .out, check the LAST "RESULT" line against EXPECTED;
  4. restart the Mac if the installer changed the System file, and return only once the agent is back;
  5. RUN MoofStat sw-<target> and check its driver line: "MoofPort refnum ..." for moofport, "... not MoofPort ..."
     for dayna (--no-verify skips this).
Exit 0 ok, 2 a wrong result (installer or driver), 3 HUNG (from moofctl), 1 a bad call.
The installer names its .out "<its own file name>-<id>.out" (the file MoofAgent launched), so the agent sees it finish
and answers `done`; anything but `done` fails. The old .out is deleted first (and checked gone), so a RESULT line is this run's.

settings: rewrites MoofBatch.cfg (line 1 "<host> <modes>", line 2 "settings <names>", optional line 3 "rbuf <bytes>", 4096..32768); parts not given keep the old
file's values. This is the core `settings` command's logic, taken over here (the core keeps its copy for one release).

The core's names (do_restart, check_run_args, save_run, AgentError) come from moofctl.py, which is __main__ under
moofctl; tests set CORE.
"""
import os
import sys

CORE = None                     # the moofctl module; None = find it (moofctl runs as __main__)

CFG_NAME = "MoofPortInstall.cfg"
BATCH_CFG = "MoofBatch.cfg"
DEFAULT_INSTALLER = "MoofPortInstall"

# RESULT texts of the installer that mean the switch went as asked: (prefix, changed, literal fragment of the C
# source). `changed`: the System file changed, so the Mac must restart. test_moofport_plugin.py greps the fragments
# in mac/installer/installer.c, so a reworded message breaks that test, not a session.
EXPECTED = {
    "moofport": [
        ("MoofPort is installed.", True, "MoofPort is installed."),
        ("MoofPort is updated.", True, "MoofPort is updated."),
    ],
    "dayna": [
        ("DaynaPORT's driver is back.", True, "DaynaPORT's driver is back."),
        ("Already DaynaPORT, nothing to do.", False, "Already DaynaPORT, nothing to do."),
    ],
}
# MoofStat's driver line (mac/moofstat/moofstat.c LOG): what the Status call found at .ENET0's refnum.
MP_FRAG = "MoofPort refnum"
NOT_MP_FRAG = "not MoofPort"


def _core():
    if CORE is not None:
        return CORE
    for m in (sys.modules.get("__main__"), sys.modules.get("moofctl")):
        if m is not None and hasattr(m, "do_restart") and hasattr(m, "check_run_args"):
            return m
    raise RuntimeError("moofport plugin: moofctl's core names not found (run it through moofctl.py)")


def lines_of(data):
    return data.decode("mac_roman").replace("\r\n", "\n").replace("\r", "\n").split("\n")


def result_line(text_lines):
    """The text after "RESULT " on the LAST RESULT line, or None."""
    res = None
    for ln in text_lines:
        if ln.startswith("RESULT "):
            res = ln[7:]
    return res


def verdict(target, result):
    """(ok, changed) for an installer RESULT text of `switch <target>`."""
    if result is None:
        return False, False
    for prefix, changed, _ in EXPECTED[target]:
        if result.startswith(prefix):
            return True, changed
    return False, False


def driver_is_moofport(text_lines):
    """True / False from MoofStat's output, None if it shows neither line."""
    if any(MP_FRAG in ln for ln in text_lines):
        return True
    if any(NOT_MP_FRAG in ln for ln in text_lines):
        return False
    return None


RBUF_MIN, RBUF_MAX = 4096, 32768      # MoofBatch's accepted TCPCreate buffer (mac/tcpsend/tcpsend.c)


def batch_cfg(old, host=None, modes=None, settings=None, rbuf=None):
    """MoofBatch.cfg text with line 1 "<host> <modes>" and line 2 "settings <names>" changed as
    given, and an optional line "rbuf <bytes>" (mac/tcpsend/tcpsend.c read_cfg). Missing parts keep the old file's values; MoofBatch's
    defaults stand in for a missing file. Mac line ends (CR)."""
    lines = old.replace("\r\n", "\r").replace("\n", "\r").split("\r") if old else []
    first = lines[0].split() if lines and lines[0].strip() else []
    h = host or (first[0] if first else None)
    if not h:
        raise ValueError("MoofBatch.cfg has no host yet: give --host")
    m = modes or (first[1] if len(first) > 1 else "EACRAC")
    s = settings
    rb = rbuf
    if rb is not None:
        try:
            ok = RBUF_MIN <= int(str(rb), 10) <= RBUF_MAX
        except ValueError:
            ok = False
        if not ok:
            raise ValueError("rbuf %r: need a number %d..%d" % (rb, RBUF_MIN, RBUF_MAX))
        rb = int(str(rb), 10)
    for ln in lines[1:]:
        if s is None and ln.startswith("settings "):
            s = ln[9:].strip()
        if rb is None and ln.startswith("rbuf "):
            rb = ln[5:].strip()
    out = "%s %s\r" % (h, m)
    if s:
        out += "settings %s\r" % s
    if rb is not None:
        out += "rbuf %s\r" % rb
    return out


def do_settings(core, agent, host=None, modes=None, settings=None, rbuf=None):
    try:
        old = agent.get(BATCH_CFG).decode("mac_roman")
    except core.AgentError:
        old = ""
    new = batch_cfg(old, host, modes, settings, rbuf)
    agent.put(BATCH_CFG, new.encode("mac_roman"))
    return new


def do_switch(core, link, agent, target, installer=DEFAULT_INSTALLER, timeout=600, back_timeout=240,
              verify=True, outdir=".", log=print):
    """Returns (exit code, agent). moofctl's Hung (no answer) is not caught here: moofctl exits 3."""
    rid = "sw-" + target
    try:
        core.check_run_args(installer, rid)
    except ValueError as e:
        log("FAILED: %s" % e)
        return 1, agent
    out_name = "%s-%s.out" % (installer, rid)       # the installer names its .out after its own file name
    try:
        agent.delete(out_name)                      # never read a stale result
    except core.AgentError:
        pass                                        # fine if it was not there: what counts is the check below
    try:
        agent.get(out_name)
        log("FAILED: the old %s is still there after DEL" % out_name)
        return 2, agent
    except core.AgentError:
        pass                                        # gone
    agent.put(CFG_NAME, b"install\r" if target == "moofport" else b"uninstall\r")
    try:
        ran, _, pc = agent.run(installer, rid, max_s=int(timeout), timeout=timeout + 120)
    except core.AgentError as e:
        log("FAILED: RUN %s %s: %s" % (installer, rid, e))
        return 2, agent
    log("%s %s: %s in %.1f s (PC clock)" % (installer, rid, ran["result"], pc["seconds"]))
    try:
        data = agent.get(out_name)
    except core.AgentError as e:
        log("FAILED: no %s: %s" % (out_name, e))
        return 2, agent
    os.makedirs(outdir, exist_ok=True)
    with open(os.path.join(outdir, out_name), "wb") as f:
        f.write(data)
    text = lines_of(data)
    res = result_line(text)
    log("installer: %s" % (text[0] if text else ""))
    log("result:    %s" % (res or "(no RESULT line)"))
    ok, changed = verdict(target, res)
    if ran["result"] != "done" or not ok:
        log("FAILED: switch %s: the installer did not do it (see %s)" % (target, os.path.join(outdir, out_name)))
        return 2, agent
    if changed:
        agent, secs = core.do_restart(link, agent, back_timeout)
        log("Mac back after %.1f s (PC clock)" % secs)
    else:
        log("nothing changed: no restart")
    if not verify:
        return 0, agent
    ran, data, pc = agent.run("MoofStat", rid, max_s=120, timeout=240)
    for path in core.save_run(outdir, "MoofStat", rid, ran, data, pc):
        log("saved %s" % path)
    lines = lines_of(data or b"")
    is_mp = driver_is_moofport(lines)
    line = next((ln for ln in lines if MP_FRAG in ln or NOT_MP_FRAG in ln), "")
    log("MoofStat: %s" % (line or "(no driver line)"))
    if ran["result"] != "done" or is_mp is None or is_mp != (target == "moofport"):
        log("FAILED: switch %s: MoofStat says the driver is %s" %
            (target, "unknown" if is_mp is None else "MoofPort" if is_mp else "not MoofPort"))
        return 2, agent
    log("switch %s: done, .ENET0 is %s" % (target, "MoofPort" if is_mp else "not MoofPort (DaynaPORT)"))
    return 0, agent


def register(sub):
    p = sub.add_parser("moofport", help="MoofPort: switch the Mac's .ENET0 driver; MoofBatch settings")
    ps = p.add_subparsers(dest="moofport_cmd", required=True)
    sw = ps.add_parser("switch", help="change the Mac's .ENET0 driver through MoofPortInstall, unattended")
    sw.add_argument("value", choices=["dayna", "moofport"], help="the driver to end up with")
    sw.add_argument("--installer", default=DEFAULT_INSTALLER, metavar="NAME",
                    help="the installer's name on the Mac (default %(default)s; MoofPortDiag for the diagnostic one)")
    sw.add_argument("--no-verify", action="store_true", help="skip the MoofStat check after the restart")
    sw.add_argument("--install-timeout", type=float, default=600, help="PC: HUNG if the installer does not answer")
    sw.add_argument("--back-timeout", type=float, default=240, help="PC: HUNG if the Mac is not back this many s after RESTART")
    st = ps.add_parser("settings", help="rewrite MoofBatch.cfg (parts not given keep their old values)")
    st.add_argument("--host", help="MoofBatch.cfg line 1: the PC's address")
    st.add_argument("--modes", help="MoofBatch.cfg line 1: the modes, e.g. EACRAC")
    st.add_argument("--settings", help='MoofBatch.cfg line 2: the settings, e.g. "new steady"')
    st.add_argument("--rbuf", help="MoofBatch.cfg line 3: the TCPCreate receive buffer, 4096..32768 (default 8192)")


def dispatch(cmd, args, link, agent, log):
    if cmd != "moofport":
        return None
    core = _core()
    if args.moofport_cmd == "settings":
        try:
            new = do_settings(core, agent, args.host, args.modes, args.settings, args.rbuf)
        except ValueError as e:
            log("FAILED: %s" % e)
            return 1, agent
        log(new.replace("\r", "\n").rstrip("\n"))
        return 0, agent
    return do_switch(core, link, agent, args.value, installer=args.installer, timeout=args.install_timeout,
                     back_timeout=args.back_timeout, verify=not args.no_verify,
                     outdir=getattr(args, "out", "."), log=log)
