#!/usr/bin/env bash
# Host-level validation, the layer above the unit tests.
#
# The unit tests drive the processor directly. These drive it the way a DAW
# does -- wrong bus layouts, silly sample rates, parameters yanked from another
# thread, state saved and restored at awkward moments. Plenty has passed the
# tests and failed here.
#
# Usage: tools/validate-plugin.sh [--strict]
#   --strict  treat a missing pluginval as a failure rather than a skip

set -uo pipefail

STRICT=0
[ "${1:-}" = "--strict" ] && STRICT=1

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VST3="$ROOT/build/motif-xs-plugin_artefacts/Release/VST3/Motif Rack XS.vst3"
AU_TYPE=aumf          # music effect: an effect that accepts MIDI
AU_SUBTYPE=Mrxs
AU_MANUFACTURER=Mtfx

failures=0
note()  { printf '\n== %s\n' "$1"; }
fail()  { printf '   FAIL %s\n' "$1"; failures=$((failures + 1)); }
pass()  { printf '   ok   %s\n' "$1"; }
skip()  { printf '   SKIP %s\n' "$1"; }

note "the rack must be free"
# Both validators instantiate the plugin, which opens the rack. If something
# else holds it the plugin still loads, but every device test underneath is
# meaningless -- so say so rather than reporting a clean run.
LOCK="$HOME/Library/Application Support/MotifRackXS/rack.lock"
if [ -f "$LOCK" ] && holder=$(cat "$LOCK" 2>/dev/null) && [ -n "$holder" ]; then
    pid=$(printf '%s' "$holder" | sed -n 's/.*(pid \([0-9]*\)).*/\1/p')
    if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
        skip "the rack is held by $holder -- close it for a meaningful run"
    else
        pass "no live holder"
    fi
else
    pass "no live holder"
fi

note "Audio Unit validation (auval)"
if command -v auval > /dev/null 2>&1; then
    if auval -v "$AU_TYPE" "$AU_SUBTYPE" "$AU_MANUFACTURER" > /tmp/auval-motifxs.log 2>&1; then
        pass "auval passed"
    else
        fail "auval failed -- see /tmp/auval-motifxs.log"
        tail -20 /tmp/auval-motifxs.log | sed 's/^/        /'
    fi
else
    skip "auval not found (it ships with macOS; this should not happen)"
fi

note "VST3 validation (pluginval)"
if command -v pluginval > /dev/null 2>&1; then
    if [ ! -d "$VST3" ]; then
        fail "no VST3 built at $VST3"
    elif pluginval --validate-in-process --strictness-level 7 \
                   --validate "$VST3" > /tmp/pluginval-motifxs.log 2>&1; then
        pass "pluginval passed at strictness 7"
    else
        fail "pluginval failed -- see /tmp/pluginval-motifxs.log"
        tail -20 /tmp/pluginval-motifxs.log | sed 's/^/        /'
    fi
else
    if [ "$STRICT" = 1 ]; then
        fail "pluginval not installed (brew install --cask pluginval)"
    else
        skip "pluginval not installed -- brew install --cask pluginval"
    fi
fi

note "result"
if [ "$failures" -eq 0 ]; then
    printf '   all host validation passed\n'
else
    printf '   %d failure(s)\n' "$failures"
fi
exit "$failures"
