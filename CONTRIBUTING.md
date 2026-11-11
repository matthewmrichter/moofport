# Contributing

Thanks for helping! Bug reports, compatibility reports ("program X does / doesn't work with MoofPort") and pull
requests are all welcome.

## Reporting a problem

Open an issue and use the bug report template. The most useful thing you can add is the `MoofStat-<name>.out` file:
run **MoofStat** right after the problem, while the Mac is still running (see [TOOLS.md](TOOLS.md)).

## Building and testing

You need gcc and CMake for the host tests, and Docker for the Mac builds. You do not need a Mac, an emulator or
Apple software for either.

```sh
cmake -S . -B build && cmake --build build
ctest --test-dir build --output-on-failure     # host tests (ASan + UBSan on by default)
scripts/build-mac.sh MoofPortInstall           # the 68k build (Retro68 in Docker); also MoofStat, MoofTune
```

The emulator (Snow) tests need your own Mac ROM and Apple system software; they are optional for contributors.
Real-hardware testing is done by the maintainer before a release. Speed claims come only from real hardware,
because Snow does not model SCSI timing.

## Pull requests

- Keep a change focused, and run the host tests first.
- `core/` rules: C99, no `malloc`, no mutable globals, all packet access through `mp_rd16/32` and `mp_wr16/32`,
  wrap-safe clock compares, no blocking, no division or 64-bit math on hot paths. Add a test for new behavior.
  `CLAUDE.md` has the details (it is written for coding agents, but it is a good map for people too).
- The driver is position-independent: no libgcc helper calls (`__divsi3`, `__mulsi3`, ...).
- Memory is a feature: resident memory must stay within the budget that `test_budget` checks. Say in the PR if your
  change makes the driver larger.
- MacTCP and AppleTalk compatibility is at the `.ENET0` interface (`docs/dev/INTERFACE.md`). Do not change what
  they can see.
- **Do not attach or commit Apple software** (ROMs, System disks, MacTCP, DaynaPORT, disk images) or built binaries.
- AI-assisted contributions are fine; you are responsible for understanding and testing what you submit.

By contributing you agree that your work is released under the repository's [MIT license](LICENSE).
