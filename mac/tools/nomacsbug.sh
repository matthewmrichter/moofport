#!/bin/sh
# nomacsbug.sh - compiler launcher for the shipping driver (mac/CMakeLists.txt): runs the compile,
# then drops Retro68's MacsBug name sections (.text.<function>.macsbug: an RTS where the function
# didn't end in one, the length byte, the name, a zero word) from the object. The names are for
# MacsBug's stack crawls only; in the resident driver they cost about 1 KB. The diagnostic driver
# keeps them.
"$@" || exit $?
obj=""
while [ $# -gt 0 ]; do
  if [ "$1" = "-o" ]; then obj="$2"; break; fi
  shift
done
[ -n "$obj" ] || exit 0
exec m68k-apple-macos-objcopy --wildcard --remove-section='.text.*.macsbug' "$obj"
