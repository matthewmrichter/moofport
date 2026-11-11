#!/usr/bin/env python3
"""test_moofport_plugin.py - scripts/moofctl-plugins/moofport.py against a minimal fake agent (only put/get/delete/
run/restart are called; a recorder shows the order). The real moofctl.py supplies the core names (do_restart,
check_run_args, save_run, AgentError); find it with $MOOFCTL_PY or ~/projects/moofagent/scripts/moofctl.py. Without it
this exits 77 (ctest: Skipped). The test lives here, not in scripts/moofctl-plugins/: moofctl imports every *.py there."""
import argparse
import contextlib
import io
import importlib.util
import os
import shutil
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, ".."))
CORE_PATH = os.environ.get("MOOFCTL_PY") or os.path.expanduser("~/projects/moofagent/scripts/moofctl.py")


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


plug = load("moofport_plugin", os.path.join(ROOT, "scripts", "moofctl-plugins", "moofport.py"))
core = load("moofctl", CORE_PATH) if os.path.exists(CORE_PATH) else None
plug.CORE = core

INSTALLED = "MoofPort is installed. Restart the Mac to use it."
UPDATED = "MoofPort is updated. Restart the Mac to use the new version."
BACK = "DaynaPORT's driver is back. Restart the Mac to use it."
NOTHING = "Already DaynaPORT, nothing to do. Nothing was changed."
NO_KEPT = "No DaynaPORT driver was kept; not removing the only Ethernet driver unattended. Nothing was changed."
REFUSED = "DRVx 47 exists but DRVR 47 is not MoofPort. Nothing was changed."
STAT_MP = b"MoofStat\rMoofPort refnum -48, SCSI ID 4, open 1, MAC 00:80:19:00:00:01\r"
STAT_DAYNA = b"MoofStat\rrefnum -48: not MoofPort (Status = -18)\r"


def out_text(result, extra=""):
    return ("MoofPortInstall 1.0 perf-harness\rSystem file ...\r%s%s" %
            (extra, "RESULT %s\r" % result if result else "")).encode("mac_roman")


class Link:
    def __init__(self, log):
        self.log = log

    def open(self, timeout, fresh=False):
        self.log.append("link.open fresh=%s" % fresh)
        return self.agent


class Agent:
    """Records every call in `log`; RUN of the installer writes Installer-<id>.out, RUN of MoofStat answers `stat`."""

    def __init__(self, log, installer_out, stat, ran_result="done", hang=False, stale=False, busy=False):
        self.log, self.installer_out, self.stat = log, installer_out, stat
        self.ran_result, self.hang, self.busy, self.stale = ran_result, hang, busy, stale
        self.files = {}

    def delete(self, name):
        self.log.append("delete %s" % name)
        if self.stale:
            self.files[name] = b"RESULT old\r"
            raise core.AgentError("in use")
        if name not in self.files:
            raise core.AgentError("no such file")
        del self.files[name]

    def put(self, name, data):
        self.log.append("put %s %r" % (name, data.decode()))
        self.files[name] = data

    def get(self, name):
        self.log.append("get %s" % name)
        if name not in self.files:
            raise core.AgentError("no such file")
        return self.files[name]

    def run(self, tool, rid, max_s=None, timeout=None):
        self.log.append("run %s %s" % (tool, rid))
        if self.hang:
            raise core.Hung("no answer")
        if self.busy:
            raise core.AgentError("already running")
        pc = {"sent": 0.0, "answered": 1.0, "seconds": 1.0}
        if tool == "MoofStat":
            return {"tool": tool, "id": rid, "result": "done"}, self.stat, pc
        self.files.pop("MoofPortInstall.cfg", None)         # the installer reads and deletes it
        if self.installer_out is not None:
            self.files["%s-%s.out" % (tool, rid)] = self.installer_out
        return {"tool": tool, "id": rid, "result": self.ran_result}, None, pc

    def restart(self):
        self.log.append("restart")


@unittest.skipIf(core is None, "moofagent's moofctl.py not found (set MOOFCTL_PY)")
class SwitchTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.tmp)
        self.log = []
        self.msgs = []

    def switch(self, target, installer_out, stat, **kw):
        agent = Agent(self.log, installer_out, stat, kw.pop("ran_result", "done"), kw.pop("hang", False),
                      kw.pop("stale", False), kw.pop("busy", False))
        link = Link(self.log)
        link.agent = agent
        rc, a = plug.do_switch(core, link, agent, target, outdir=self.tmp, log=self.msgs.append, **kw)
        self.assertIs(a, agent)
        return rc

    def calls(self, prefix):
        return [c for c in self.log if c.startswith(prefix)]

    def test_switch_moofport(self):
        rc = self.switch("moofport", out_text(INSTALLED), STAT_MP)
        self.assertEqual(rc, 0, self.msgs)
        self.assertIn("put MoofPortInstall.cfg 'install\\r'", self.log)
        self.assertEqual(len(self.calls("restart")), 1)
        self.assertEqual(self.log.index("link.open fresh=True"), self.log.index("restart") + 1)
        order = [self.log.index(c) for c in ("delete MoofPortInstall-sw-moofport.out", "get MoofPortInstall-sw-moofport.out")]
        order += [self.log.index(self.calls(p)[0]) for p in ("put", "run MoofPortInstall", "restart", "run MoofStat")]
        self.assertEqual(order[0], 0)
        self.assertEqual(order[2:], sorted(order[2:]))
        self.assertIn("run MoofPortInstall sw-moofport", self.log)
        self.assertTrue(os.path.exists(os.path.join(self.tmp, "MoofPortInstall-sw-moofport.out")))
        self.assertTrue(os.path.exists(os.path.join(self.tmp, "MoofStat-sw-moofport.out")))

    def test_update_counts_and_restarts(self):
        self.assertEqual(self.switch("moofport", out_text(UPDATED), STAT_MP), 0, self.msgs)
        self.assertEqual(len(self.calls("restart")), 1)

    def test_switch_dayna_back_with_diag_installer(self):
        rc = self.switch("dayna", out_text(BACK), STAT_DAYNA, installer="MoofPortDiag")
        self.assertEqual(rc, 0, self.msgs)
        self.assertIn("put MoofPortInstall.cfg 'uninstall\\r'", self.log)       # the cfg name is the app's, not the file's
        self.assertIn("run MoofPortDiag sw-dayna", self.log)
        self.assertEqual(len(self.calls("restart")), 1)

    def test_nothing_to_do_no_restart_but_still_verified(self):
        self.assertEqual(self.switch("dayna", out_text(NOTHING), STAT_DAYNA), 0, self.msgs)
        self.assertEqual(self.calls("restart"), [])
        self.assertEqual(len(self.calls("run MoofStat")), 1)

    def test_no_verify_skips_moofstat(self):
        self.assertEqual(self.switch("dayna", out_text(NOTHING), STAT_DAYNA, verify=False), 0)
        self.assertEqual(self.calls("run MoofStat"), [])

    def test_refused_result_fails_without_restart(self):
        for res in (NO_KEPT, REFUSED, "Cancelled. Nothing was changed.", None,
                    "Unattended run without a valid MoofPortInstall.cfg (install or uninstall). Nothing was changed."):
            self.log.clear()
            self.assertEqual(self.switch("moofport", out_text(res), STAT_MP), 2, res)
            self.assertEqual(self.calls("restart"), [])
            self.assertEqual(self.calls("run MoofStat"), [])

    def test_wrong_driver_after_restart_fails(self):
        self.assertEqual(self.switch("moofport", out_text(INSTALLED), STAT_DAYNA), 2)
        self.assertEqual(len(self.calls("restart")), 1)
        self.assertEqual(self.switch("dayna", out_text(BACK), STAT_MP), 2)

    def test_moofstat_with_no_driver_line_fails(self):
        self.assertEqual(self.switch("moofport", out_text(INSTALLED), b"MoofStat\r"), 2)
        self.assertEqual(self.switch("dayna", out_text(NOTHING), None), 2)

    def test_verdict_is_from_the_last_result_line(self):
        out = out_text(INSTALLED, extra="RESULT Cancelled. Nothing was changed.\r")        # last one wins: INSTALLED
        self.assertEqual(self.switch("moofport", out, STAT_MP), 0, self.msgs)
        out = out_text(REFUSED, extra="RESULT %s\r" % INSTALLED)
        self.assertEqual(self.switch("moofport", out, STAT_MP), 2)

    def test_missing_out_file_is_exit_2(self):
        self.assertEqual(self.switch("moofport", None, STAT_MP), 2)

    def test_old_out_that_cannot_be_deleted_is_exit_2(self):
        self.assertEqual(self.switch("moofport", out_text(INSTALLED), STAT_MP, stale=True), 2)
        self.assertEqual(self.calls("run"), [])
        self.assertEqual(self.calls("put"), [])

    def test_nostart_is_a_failure(self):             # the installer names its .out after itself: the agent sees it, so `done`
        self.assertEqual(self.switch("moofport", out_text(INSTALLED), STAT_MP, ran_result="nostart"), 2)
        self.assertEqual(self.calls("restart"), [])

    def test_out_is_named_after_the_installer_file(self):
        self.assertEqual(self.switch("dayna", out_text(BACK), STAT_DAYNA, installer="MoofPortDiag"), 0, self.msgs)
        self.assertIn("get MoofPortDiag-sw-dayna.out", self.log)
        self.assertTrue(os.path.exists(os.path.join(self.tmp, "MoofPortDiag-sw-dayna.out")))

    def test_other_run_results_fail(self):
        self.assertEqual(self.switch("moofport", out_text(INSTALLED), STAT_MP, ran_result="timeout"), 2)
        self.assertEqual(self.calls("restart"), [])

    def test_run_refused_is_exit_2(self):
        self.assertEqual(self.switch("moofport", out_text(INSTALLED), STAT_MP, busy=True), 2)
        self.assertEqual(self.calls("restart"), [])

    def test_hung_propagates(self):
        with self.assertRaises(core.Hung):
            self.switch("moofport", out_text(INSTALLED), STAT_MP, hang=True)

    def test_bad_installer_name_is_exit_1(self):
        self.assertEqual(self.switch("moofport", out_text(INSTALLED), STAT_MP, installer="Moof Port"), 1)
        self.assertEqual(self.calls("put"), [])


@unittest.skipIf(core is None, "moofagent's moofctl.py not found (set MOOFCTL_PY)")
class SettingsTests(unittest.TestCase):
    def test_batch_cfg(self):
        self.assertEqual(plug.batch_cfg("", host="192.168.50.190"), "192.168.50.190 EACRAC\r")
        old = "192.168.50.190 EACRAC\rsettings new oldhush\r"
        self.assertEqual(plug.batch_cfg(old, settings="steady new-txpolled"),
                         "192.168.50.190 EACRAC\rsettings steady new-txpolled\r")
        self.assertEqual(plug.batch_cfg(old, modes="R"), "192.168.50.190 R\rsettings new oldhush\r")
        self.assertEqual(plug.batch_cfg("10.0.0.1 E\nsettings a\n", host="10.0.0.9"), "10.0.0.9 E\rsettings a\r")
        with self.assertRaises(ValueError):
            plug.batch_cfg("", settings="new")

    def test_same_as_the_core_copy(self):
        for args in (("", "1.2.3.4", None, None), ("5.6.7.8 R\rsettings a b\r", None, "EA", None),
                     ("5.6.7.8 R\rsettings a b\r", None, None, "c")):
            self.assertEqual(plug.batch_cfg(*args), core.batch_cfg(*args))

    def test_dispatch_settings_writes_the_cfg(self):
        log, msgs = [], []
        agent = Agent(log, None, None)
        agent.files["MoofBatch.cfg"] = b"10.0.0.1 EA\rsettings new\r"
        ap = argparse.ArgumentParser()
        sub = ap.add_subparsers(dest="cmd")
        plug.register(sub)
        a = ap.parse_args(["moofport", "settings", "--settings", "steady"])
        self.assertEqual(plug.dispatch("moofport", a, None, agent, msgs.append), (0, agent))
        self.assertEqual(agent.files["MoofBatch.cfg"], b"10.0.0.1 EA\rsettings steady\r")
        agent2 = Agent([], None, None)                          # no cfg yet and no --host: a bad call
        self.assertEqual(plug.dispatch("moofport", a, None, agent2, msgs.append)[0], 1)
        self.assertNotIn("MoofBatch.cfg", agent2.files)


class RegisterTests(unittest.TestCase):
    def test_register_and_dispatch(self):
        ap = argparse.ArgumentParser()
        sub = ap.add_subparsers(dest="cmd")
        plug.register(sub)
        a = ap.parse_args(["moofport", "switch", "dayna", "--installer", "Inst", "--no-verify"])
        self.assertEqual((a.cmd, a.moofport_cmd, a.value, a.installer, a.no_verify), ("moofport", "switch", "dayna", "Inst", True))
        self.assertIsNone(plug.dispatch("other", a, None, None, print))
        with self.assertRaises(SystemExit), contextlib.redirect_stderr(io.StringIO()):
            ap.parse_args(["moofport", "switch", "dayna2"])


def slurp(rel):
    with open(os.path.join(ROOT, rel), encoding="utf-8") as f:
        return f.read()


class TableTests(unittest.TestCase):
    """The expected texts come from the C source: a reworded message breaks this test, not a session."""

    def test_result_fragments_are_in_the_installer(self):
        src = slurp("mac/installer/installer.c")
        for target, rows in plug.EXPECTED.items():
            for prefix, changed, frag in rows:
                self.assertEqual(prefix, frag)
                self.assertIn('"%s' % frag, src, "%s: %r is not the start of a string in installer.c" % (target, frag))

    def test_refusal_texts_are_in_the_installer(self):
        src = slurp("mac/installer/installer.c")
        self.assertIn(NO_KEPT, src)                                  # one string literal, whole

    def test_cfg_name_and_out_name_are_the_installers(self):
        src = slurp("mac/installer/installer.c")
        self.assertIn('\\p%s"' % plug.CFG_NAME, src)
        self.assertIn('"unattended: no %s' % plug.CFG_NAME, src)
        self.assertIn("own_name", src)                                # the .out is named after the running app
        self.assertIn('"uninstall"', src)                             # the words read_auto() takes
        self.assertIn('"install"', src)

    def test_moofstat_driver_lines_are_in_moofstat(self):
        src = slurp("mac/moofstat/moofstat.c")
        self.assertIn('"%s %%d' % plug.MP_FRAG, src)
        self.assertIn('%%d: %s' % plug.NOT_MP_FRAG, src)

    def test_verdict_table(self):
        self.assertEqual(plug.verdict("moofport", INSTALLED), (True, True))
        self.assertEqual(plug.verdict("moofport", UPDATED), (True, True))
        self.assertEqual(plug.verdict("dayna", INSTALLED), (False, False))
        self.assertEqual(plug.verdict("dayna", BACK), (True, True))
        self.assertEqual(plug.verdict("dayna", NOTHING), (True, False))
        self.assertEqual(plug.verdict("dayna", "MoofPort is uninstalled. The Mac has no Ethernet driver now. Restart the Mac."),
                         (False, False))
        self.assertEqual(plug.verdict("dayna", NO_KEPT), (False, False))
        self.assertEqual(plug.verdict("dayna", "MoofPort is uninstalled. The Mac has no Ethernet driver now. Restart the Mac."),
                         (False, False))
        self.assertEqual(plug.verdict("moofport", None), (False, False))


if __name__ == "__main__":
    if core is None:           # ctest SKIP_RETURN_CODE 77: shown as "Skipped", not as a pass
        print("SKIPPED: moofagent's moofctl.py not found at %s (set MOOFCTL_PY)" % CORE_PATH)
        sys.exit(77)
    unittest.main()
