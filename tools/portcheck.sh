#!/bin/sh
# Generate a coverage report of the quakevr -> vkQuake VR port.
#
# Every number here is counted from the two source trees at the moment it runs.
# Nothing is hand-maintained, so it cannot drift from reality the way a prose
# tracker does -- which is exactly how this port came to claim "parity" while
# the grab and reload commands did not exist.
#
# Usage: sh tools/portcheck.sh [path-to-quakevr]

SRC="${1:-E:/quakevr}"
SQ="$SRC/Quake"
DQ="$(dirname "$0")/../Quake"
QC="$(dirname "$0")/../VRQC"
CFG="$SRC/ReleaseFiles/Id1/config.cfg"

section () { printf '\n== %s ==\n' "$1"; }
report () { # name, missing-file
  n=$(wc -l < "$2" | tr -d ' ')
  if [ "$n" = "0" ]; then printf '  %-28s OK\n' "$1"
  else printf '  %-28s %s missing\n' "$1" "$n"; sed 's/^/      /' "$2"; fi
}

T=$(mktemp -d)

section "CVARS"
tr '\n' ' ' < "$SQ/vr_cvars.cpp" | grep -oP 'DEFINE_[A-Z_]+\(\s*\K[a-z0-9_]+' | sort -u > "$T/a"
grep -ohP 'cvar_t\s+\w+\s*=\s*\{\s*"\K[^"]+' "$DQ"/*.c | sort -u > "$T/b"
comm -23 "$T/a" "$T/b" | grep -v '^name$' > "$T/m"; report "globals" "$T/m"

section "BUILTINS"
grep -rhoP '\b\w+\s*=\s*#\K\d+' "$QC"/*.qc 2>/dev/null | sort -nu > /dev/null
grep -rhoP '\b\w+\s*=\s*#\d+' "$QC"/*.qc 2>/dev/null | sed 's/\s*=\s*#/ /' | sort -u -k2 -n | awk '$2>=79 {print $1}' | sort -u > "$T/a"
: > "$T/m"
while read -r n; do grep -q "\"$n\"" "$DQ"/pr_ext.c "$DQ"/pr_cmds.c 2>/dev/null || echo "$n" >> "$T/m"; done < "$T/a"
report "quakevr range (79+)" "$T/m"

section "CONSOLE COMMANDS"
grep -rhoP 'Cmd_AddCommand\s*\(\s*"\K[^"]+' "$SQ"/*.cpp | sort -u > "$T/a"
grep -rhoP 'Cmd_AddCommand\s*\(\s*"\K[^"]+' "$DQ"/*.c | sort -u > "$T/b"
comm -23 "$T/a" "$T/b" > "$T/m"; report "commands" "$T/m"

section "DEFAULT BINDINGS"
# every command the shipped config binds, that this engine does not register
grep '^bind ' "$CFG" 2>/dev/null | sed 's/.*" "//; s/"$//' | sed 's/^[+-]//' | awk '{print $1}' | sort -u > "$T/a"
: > "$T/m"
while read -r c; do
  case "$c" in impulse|echo|wait|save|load) continue;; esac
  grep -q "\"$c\"\|\"+$c\"" "$DQ"/*.c 2>/dev/null || echo "$c" >> "$T/m"
done < "$T/a"
report "bound commands" "$T/m"

section "STATS"
# quakevr declares these as #define, vkQuake as an enum. Matching only one form
# reports every stock stat as missing, which buries the handful that really are
# -- a report that cries wolf is worse than no report.
grep -ohP 'STAT_\K[A-Z0-9_]+' "$SQ"/*.hpp "$SQ"/*.h 2>/dev/null | sort -u > "$T/a"
grep -ohP 'STAT_\K[A-Z0-9_]+' "$DQ"/*.h "$DQ"/*.c 2>/dev/null | sort -u > "$T/b"
comm -23 "$T/a" "$T/b" > "$T/m"; report "STAT_*" "$T/m"

section "QC ENTITY FIELDS"
# Only fields quakevr's ENGINE touches matter. A field the QC declares and only
# the QC uses needs nothing from the engine -- listing those buries the ones
# that do. quakevr welds its VR fields into entvars_t and reaches them as
# ed->v.name, so that access pattern is the enumeration.
#
# On this side a field is satisfied either by entvars_t or by a QCEXTFIELD
# named lookup, which is how this port keeps PROGHEADER_CRC at stock 5927.
grep -rhoP '->v\.\K\w+' "$SQ"/*.cpp 2>/dev/null | sort -u > "$T/a"
{ grep -ohP 'QCEXTFIELD\s*\(\s*\K\w+' "$DQ"/progs.h
  # entvars_t is generated into progdefs.q1, not written out in progs.h
  grep -ohP '^\s+\w+\s+\K\w+(?=\s*;)' "$DQ"/progdefs.q1 2>/dev/null
} | sort -u > "$T/b"
comm -23 "$T/a" "$T/b" > "$T/m"; report "fields quakevr engine reads" "$T/m"

section "ASSETS REFERENCED BY QC"
grep -rhoP '"\K(progs|sound|maps)/[a-z0-9_/]+\.(mdl|spr|bsp|wav)' "$QC"/*.qc 2>/dev/null | sort -u > "$T/a"
printf '  %s asset paths referenced by the ported QuakeC\n' "$(wc -l < "$T/a" | tr -d ' ')"

printf '\n(counted %s)\n' "$(date 2>/dev/null || echo now)"
rm -rf "$T"
