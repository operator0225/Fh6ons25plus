#!/bin/sh
# Measure how far Xodus is from launching a Game Pass title.
#
# The Game Pass blocker is upstream and has no announced timeline ("soontm"),
# so press coverage is a poor way to track it. This counts the things that
# actually have to change, straight from the source.
#
# Two halves have to meet:
#   game.exe -> xgameruntime.dll (C, Wine side) -> unix socket -> xodus-service (Rust, host)
#
# Neither half is finished, and as of the first run they are not connected.
#
#   ./tools/xodus-progress.sh            # clone/update into a cache dir, report
#   ./tools/xodus-progress.sh --dir DIR  # use DIR as the cache
#
# No arguments needed. Needs git and standard POSIX tools.

set -eu

DIR="${TMPDIR:-/tmp}/xodus-progress"
while [ $# -gt 0 ]; do
    case "$1" in
        --dir) DIR="$2"; shift 2 ;;
        -h|--help) sed -n '2,18p' "$0"; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

fetch() {
    # $1 = repo name under github.com/xodus-gaming
    if [ -d "$DIR/$1/.git" ]; then
        git -C "$DIR/$1" fetch --depth 1 --quiet origin 2>/dev/null &&
        git -C "$DIR/$1" reset --hard --quiet origin/HEAD 2>/dev/null ||
        git -C "$DIR/$1" pull --quiet 2>/dev/null || true
    else
        git clone --depth 1 --quiet "https://github.com/xodus-gaming/$1" "$DIR/$1"
    fi
}

# Count matches across files, printing 0 rather than nothing when there are none.
count() {
    pat="$1"; shift
    [ "$#" -eq 0 ] && { echo 0; return; }
    grep -ho "$pat" "$@" 2>/dev/null | wc -l | tr -d ' '
}

mkdir -p "$DIR"
echo "Xodus progress -- $(date -u '+%Y-%m-%d %H:%M UTC')"
echo "cache: $DIR"
echo

for r in xgameruntime xodus; do
    printf 'fetching %s ... ' "$r"
    if fetch "$r" 2>/dev/null; then
        echo "$(git -C "$DIR/$r" log -1 --format='%h %ad' --date=short 2>/dev/null || echo ok)"
    else
        echo "FAILED (network?) -- using whatever is cached"
    fi
done
echo

# ---- Wine side: xgameruntime.dll -------------------------------------------
GR="$DIR/xgameruntime"
if [ -d "$GR" ]; then
    set -- "$GR"/*.c
    [ -e "$1" ] || set --
    notimpl=$(count 'E_NOTIMPL' "$@")
    fixme=$(count 'FIXME' "$@")
    ok=$(count 'return S_OK' "$@")
    total=$((notimpl + ok))

    echo "== Wine side: xgameruntime.dll (C) =="
    echo "   E_NOTIMPL      : $notimpl   <- functions that do nothing yet"
    echo "   return S_OK    : $ok        <- mostly per-module registration, not real work"
    echo "   FIXME          : $fixme"
    if [ "$total" -gt 0 ]; then
        echo "   unimplemented  : $((notimpl * 100 / total))% of E_NOTIMPL+S_OK returns"
    fi

    # The two halves have to talk over $XDG_RUNTIME_DIR/xodus.sock. Until the C
    # side has a socket client, the Wine half cannot reach the Rust half at all.
    if grep -rqiE 'xodus|AF_UNIX|sys/socket\.h' "$GR"/*.c "$GR"/*.h 2>/dev/null; then
        echo "   IPC to service : present"
    else
        echo "   IPC to service : ABSENT -- Wine side cannot reach xodus-service"
    fi
    echo
fi

# ---- Host side: xodus-service ----------------------------------------------
XO="$DIR/xodus"
if [ -d "$XO" ]; then
    echo "== Host side: xodus-service (Rust) =="
    proto="$XO/crates/xodus/proto/xodus/common.proto"
    if [ -f "$proto" ]; then
        # Message types define what the game can ask for. PING/PONG are plumbing,
        # so the real count is the request/response pairs beyond them.
        ops=$(grep -cE '^\s+[A-Z_]+\s*=\s*[0-9]+;' "$proto" || echo 0)
        echo "   protocol ops   : $ops (incl. UNKNOWN/PING/PONG plumbing)"
        grep -oE '^\s+[A-Z_]+' "$proto" | tr -d ' ' | sed 's/^/                    /'
    fi
    if grep -rq 'unimplemented!' "$XO/crates/xodus-service/src" 2>/dev/null; then
        echo "   protobuf path  : unimplemented!()"
    else
        echo "   protobuf path  : implemented"
    fi
    echo
fi

cat <<'EOF'
Read it like this: the scaffold is real -- every GDK module has an IDL and a C
file -- but the bodies are empty and the two halves are not wired together.
"Done" looks like E_NOTIMPL near zero, IPC present, and the protocol carrying
XUser/XStore/XPackage operations rather than PING and one token request.

Even then it is x86_64 Linux. Running it here additionally needs those Wine
patches carried into Winlator's ARM64 Android Wine, under Box64 or FEXCore --
a second port that has not been started. See docs/01-gamepass-msstore.md.
EOF
