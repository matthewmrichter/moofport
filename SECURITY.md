# Security policy

MoofPort is a network driver for 68k Macs. A bug in it can be a security bug, for example a crash or memory
corruption caused by a crafted packet.

## Supported versions

Only the latest release (1.0 at the time of writing) is supported.

## Reporting a vulnerability

Do **not** open a public issue for a security problem. Use GitHub's private reporting instead:
**Security tab, "Report a vulnerability"** on this repository. Tell us what you saw, the packet or program that
causes it if you know it, and your Mac, System version and BlueSCSI firmware version. A `MoofStat-<name>.out`
file (see [TOOLS.md](TOOLS.md)) helps.

This is a hobby project run by one person. There is no guaranteed response time, but reports are read and taken
seriously.

## Scope

In scope: the portable core (`core/`), the `.ENET0` driver (`mac/driver/`), MoofPortInstall, MoofStat and MoofTune.
Out of scope: bugs in the BlueSCSI firmware, in Apple's System software, MacTCP or AppleTalk, in DaynaPORT, or in
third-party programs that use MoofPort.
