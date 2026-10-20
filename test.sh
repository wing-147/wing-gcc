#!/usr/bin/env bash
#
# Compare wing-gcc diagnostics against gcc on the same sources.
#
# wing-gcc is only a front end (lexer) for now, so it cannot reject everything
# gcc rejects. This harness measures how close we are instead of pinning our
# own output:
#
#   match   : both reject, and wing-gcc's first diagnostic equals gcc's
#   differ  : both reject, but the first diagnostics differ
#   missed  : gcc rejects, wing-gcc accepts (expected until the parser lands)
#   FALSE+  : gcc accepts, but wing-gcc rejects -> must be zero
#
# LC_ALL=C keeps gcc's quoting (ASCII ' ') consistent with wing-gcc's.
#
# Usage:
#   ./test.sh [-v] [file ...]             explicit files
#   ./test.sh [-v] -my                    tests/tests/**/*.c (default)
#   ./test.sh [-v] -third-party <chapter> [dir]
#       tests/third_party/writing-a-c-compiler-tests/tests/chapter_<chapter>/[dir]/*.c
#       (with no dir, every subdirectory of the chapter)
#
# -v prints the full gcc and wing-gcc output for every file.

set -u
shopt -s nullglob globstar

export LC_ALL=C

CC=${CC:-gcc}
WING=${WING:-./bin/wing-gcc}
TP=tests/third_party/writing-a-c-compiler-tests/tests

usage() {
    printf 'usage: %s [-v] [file ...]\n' "$0" >&2
    printf '       %s [-v] -my\n' "$0" >&2
    printf '       %s [-v] -third-party <chapter> [dir]\n' "$0" >&2
}

if ! command -v "$CC" >/dev/null 2>&1; then
    echo "error: C compiler '$CC' not found" >&2
    exit 1
fi

if [ ! -x "$WING" ]; then
    echo "error: $WING not built (run 'make')" >&2
    exit 1
fi

verbose=0
mode=0
files=()

while [ "$#" -gt 0 ]; do
    case $1 in
    -v) verbose=1 ;;
    -my)
        mode=1
        files=(tests/tests/**/*.c)
        ;;
    -third-party)
        if [ "$#" -lt 2 ]; then
            usage
            exit 2
        fi
        chapter=$2
        shift 2
        case $chapter in
        chapter_*) ;;
        *) chapter=chapter_$chapter ;;
        esac
        dir=
        if [ "$#" -gt 0 ] && [ "${1#-}" = "$1" ]; then
            dir=$1
            shift
        fi
        path=$TP/$chapter${dir:+/$dir}
        if [ ! -d "$path" ]; then
            echo "error: no such test directory: $path" >&2
            exit 1
        fi
        mode=1
        files=("$path"/**/*.c)
        ;;
    -h | --help)
        usage
        exit 0
        ;;
    -*)
        echo "error: unknown option '$1'" >&2
        usage
        exit 2
        ;;
    *) files+=("$1") ;;
    esac
    shift
done

if [ "${#files[@]}" -eq 0 ] && [ "$mode" -eq 0 ]; then
    files=(tests/tests/**/*.c)
fi
if [ "${#files[@]}" -eq 0 ]; then
    echo "error: no test files found" >&2
    exit 1
fi

# Color only for -v on a terminal; redirected logs stay plain text.
c_off= c_red= c_grn= c_yel= c_cyn= c_mag= c_dim=
if [ "$verbose" -eq 1 ] && [ -t 1 ]; then
    c_off=$'\033[0m' c_red=$'\033[1;31m' c_grn=$'\033[32m' c_yel=$'\033[33m'
    c_cyn=$'\033[36m' c_mag=$'\033[35m' c_dim=$'\033[2m'
fi

# First diagnostic line (empty if none).
first_diag() { grep -m1 -E 'error:|warning:' || true; }

agree=0
match=0
differ=0
missed=0
false_pos=0

for f in "${files[@]}"; do
    # -w keeps warnings out of the way so exit status reflects errors.
    gcc_out=$("$CC" -fsyntax-only -w "$f" 2>&1 >/dev/null)
    gcc_rc=$?
    wing_out=$("$WING" "$f" 2>&1 >/dev/null)
    wing_rc=$?

    if [ "$verbose" -eq 1 ]; then
        printf -- '%s--- gcc  %s%s\n%s%s\n' "$c_cyn" "$f" "$c_off" "$c_dim" "$gcc_out"
        printf -- '%s--- wing %s%s\n%s%s\n' "$c_mag" "$f" "$c_off" "$c_dim" "$wing_out"
    fi

    gcc_diag=$(printf '%s\n' "$gcc_out" | first_diag)
    wing_diag=$(printf '%s\n' "$wing_out" | first_diag)

    if [ "$gcc_rc" -eq 0 ] && [ "$wing_rc" -eq 0 ]; then
        printf '%sok      %s%s\n' "$c_grn" "$f" "$c_off"
        agree=$((agree + 1))
    elif [ "$gcc_rc" -ne 0 ] && [ "$wing_rc" -ne 0 ]; then
        if [ "$gcc_diag" = "$wing_diag" ]; then
            printf '%smatch   %s%s\n' "$c_grn" "$f" "$c_off"
            printf '%s        %s%s\n' "$c_dim" "$wing_diag" "$c_off"
            match=$((match + 1))
        else
            printf '%sdiffer  %s%s\n' "$c_yel" "$f" "$c_off"
            printf '%s    gcc : %s%s\n' "$c_dim" "$gcc_diag" "$c_off"
            printf '%s    wing: %s%s\n' "$c_dim" "$wing_diag" "$c_off"
            differ=$((differ + 1))
        fi
    elif [ "$gcc_rc" -eq 0 ]; then
        printf '%sFALSE+  %s (gcc accepts, wing-gcc rejects)%s\n' "$c_red" "$f" "$c_off"
        printf '%s    wing: %s%s\n' "$c_dim" "$wing_diag" "$c_off"
        false_pos=$((false_pos + 1))
    else
        printf '%smissed  %s (gcc rejects, wing-gcc accepts)%s\n' "$c_cyn" "$f" "$c_off"
        printf '%s    gcc : %s%s\n' "$c_dim" "$gcc_diag" "$c_off"
        missed=$((missed + 1))
    fi
done

echo
echo "summary: agree=$agree  match=$match  differ=$differ  missed=$missed  false-positives=$false_pos"
echo "(false-positives must be 0; 'missed' shrinks as language support grows)"

[ "$false_pos" -eq 0 ]
