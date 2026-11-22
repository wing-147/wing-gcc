#!/usr/bin/env python3
#
# Compare wing-gcc diagnostics against gcc on the same sources.
#
# Both compilers run their full front end (preprocess/lex/parse) on the same
# sources. Diagnostics are split into blocks (message line + source line + caret
# line + suggestion lines) and matched one-to-one: a verdict only counts when
# both sides have the same blocks.
#
#   match   : both reject and the block sets are byte-identical
#   near    : both reject, every wing-gcc block corresponds to a gcc one after
#             normalization (ASCII quotes, trailing " token") or by line:column
#             alone with the rendered body unchanged, and no gcc block is left
#   differ  : both reject but the block sets do not correspond; the verdict names
#             the reasons (body-missing / body-extra / body-differs /
#             no-counterpart / missing-diagnostic)
#   missed  : gcc rejects, wing-gcc accepts (expected until more phases land)
#   FALSE+  : gcc accepts, but wing-gcc rejects (parser coverage debt)
#
# LC_ALL=C keeps gcc's quoting (ASCII ' ') consistent with wing-gcc's.
# The oracle is N1570: gcc runs -std=c11 -pedantic-errors and explicitly
# disables `$` identifiers, which GCC otherwise retains as a target extension
# even in pedantic mode. Do not add -w here: it silences the diagnostics
# -pedantic-errors exists to produce.
#
# Usage:
#   ./test.py --self-test                 diagnostic matcher unit tests
#   ./test.py [-v] [file ...]             explicit files
#   ./test.py [-v] -my                    tests/tests/**/*.c (default)
#   ./test.py [-v] -third-party <chapter> [dir]
#       tests/third_party/writing-a-c-compiler-tests/tests/chapter_<chapter>/[dir]/*.c
#       (with no dir, every subdirectory of the chapter)
#
# Without -v only the per-file verdict line is printed. -v additionally dumps
# the full gcc and wing-gcc output for every file, colored on a terminal.

import glob
import os
import re
import shutil
import subprocess
import sys

CC = os.environ.get("CC", "gcc")
WING = os.environ.get("WING", "./bin/wing-gcc")
TP = "tests/third_party/writing-a-c-compiler-tests/tests"

DIAG_RE = re.compile(r"^.*?:(\d+):(\d+): (error|warning): (.*)$")
CONTEXT_RE = re.compile(r"^[^:]+: In (?:function|file) .*:$")
NOTE_RE = re.compile(r"^.*?:\d+:\d+: note:")

# Entry tiers, best first. Header tiers keep the diagnostic body identical;
# body tiers explain why an otherwise corresponding diagnostic still differs.
EXACT, HEADER_NEAR, LOCATION = range(3)
BODY_MISSING, BODY_EXTRA, BODY_DIFFERS, NO_COUNTERPART = range(3, 7)


def usage(prog):
    sys.stderr.write(f"usage: {prog} --self-test\n")
    sys.stderr.write(f"usage: {prog} [-v] [file ...]\n")
    sys.stderr.write(f"       {prog} [-v] -my\n")
    sys.stderr.write(f"       {prog} [-v] -third-party <chapter> [dir] [--no-extra-credit]\n")


def collect_my_tests():
    """Fixtures the default harness judges: warning_parse is still reserved."""
    files = []
    for path in glob.glob("tests/tests/**/*.c", recursive=True):
        if "/warning_parse/" in path:
            continue
        files.append(path)
    return sorted(files)


def split_diagnostics(output):
    """Split compiler output into full diagnostic blocks.

    A block is the location/message line plus the source line, caret line and
    the suggestion lines gcc prints below it. gcc's "In function 'x':" header
    and note: blocks are not part of the block and are skipped.
    """
    blocks = []
    current = None
    for line in output.splitlines():
        match = DIAG_RE.match(line)
        if match:
            current = [int(match.group(1)), int(match.group(2)), match.group(3),
                       match.group(4), []]
            blocks.append(current)
            continue
        if CONTEXT_RE.match(line) or NOTE_RE.match(line):
            current = None
            continue
        if current is not None:
            current[4].append(line.rstrip())

    entries = []
    for line, column, severity, message, body in blocks:
        while body and not body[-1]:
            body.pop()
        entries.append((line, column, severity, message, tuple(body)))
    return entries


def normalize(message):
    """Fold gcc/wing-gcc spelling differences that carry no meaning."""
    text = message.replace("\u2018", "'").replace("\u2019", "'")
    text = re.sub(r"\s+token$", "", text)
    text = text.replace("'", "")
    return re.sub(r"\s+", " ", text).strip()


def same_location(left, right):
    return left[0] == right[0] and left[1] == right[1] and left[2] == right[2]


def same_header(left, right, normalized):
    if not same_location(left, right):
        return False
    if normalized:
        return normalize(left[3]) == normalize(right[3])
    return left[3] == right[3]


def is_prefix(short, long):
    return len(short) < len(long) and long[:len(short)] == short


def match_diagnostics(wing_entries, gcc_entries):
    """Pair every wing diagnostic with a gcc one; return tiers and gcc leftovers."""
    unused = list(gcc_entries)
    tiers = []
    for entry in wing_entries:
        tier = NO_COUNTERPART

        for candidate, other in enumerate(unused):
            if entry == other:
                tier = EXACT
                del unused[candidate]
                break
        if tier != NO_COUNTERPART:
            tiers.append(tier)
            continue

        # Header-only differences are near when the rendered body agrees.
        for candidate, other in enumerate(unused):
            if same_header(entry, other, True) and entry[4] == other[4]:
                tier = HEADER_NEAR
                del unused[candidate]
                break
        if tier != NO_COUNTERPART:
            tiers.append(tier)
            continue
        for candidate, other in enumerate(unused):
            if same_location(entry, other) and entry[4] == other[4]:
                tier = LOCATION
                del unused[candidate]
                break
        if tier != NO_COUNTERPART:
            tiers.append(tier)
            continue

        # Same diagnostic, different body: classify how it differs.
        candidates = [index for index, other in enumerate(unused)
                      if same_location(entry, other) and same_header(entry, other, True)]
        paired = None
        for candidate in candidates:
            other = unused[candidate]
            if is_prefix(entry[4], other[4]):
                tier = BODY_MISSING
                paired = candidate
                break
            if is_prefix(other[4], entry[4]):
                tier = BODY_EXTRA
                paired = candidate
                break
        if tier == NO_COUNTERPART:
            for candidate in candidates:
                if unused[candidate][4] != entry[4]:
                    tier = BODY_DIFFERS
                    paired = candidate
                    break
        if paired is not None:
            del unused[paired]
        tiers.append(tier)
    return tiers, len(unused)


def run(cmd):
    env = dict(os.environ, LC_ALL="C")
    proc = subprocess.run(cmd, capture_output=True, text=True, env=env)
    return proc.returncode, proc.stderr


def pick_colors(enabled):
    if not enabled:
        return dict(off="", red="", grn="", yel="", cyn="", mag="", dim="")
    return dict(off="\033[0m", red="\033[1;31m", grn="\033[32m",
                yel="\033[33m", cyn="\033[36m", mag="\033[35m",
                dim="\033[2m")


def diagnostic_verdict(gcc_rc, wing_rc, gcc_entries, wing_entries):
    if gcc_rc != 0 and wing_rc == 0:
        return "missed", (), 0
    if gcc_rc == 0 and wing_rc != 0:
        return "false_pos", (), 0
    if gcc_rc == 0 and wing_rc == 0 and not gcc_entries and not wing_entries:
        return "agree", (), 0

    tiers, missing = match_diagnostics(wing_entries, gcc_entries)
    if missing == 0 and tiers and all(tier == EXACT for tier in tiers):
        return "match", tuple(tiers), 0
    if missing == 0 and tiers and all(tier in (EXACT, HEADER_NEAR, LOCATION) for tier in tiers):
        return "near", tuple(tiers), 0
    return "differ", tuple(tiers), missing


def run_self_tests():
    exact = (1, 2, "warning", "same warning", ("source", "caret"))
    near = (2, 3, "warning", "expected ';' token", ("source", "caret"))
    gcc_near = (2, 3, "warning", "expected ';'", ("source", "caret"))

    assert diagnostic_verdict(0, 0, [], [])[:1] == ("agree",)
    assert diagnostic_verdict(0, 0, [exact], [])[:1] == ("differ",)
    assert diagnostic_verdict(0, 0, [exact], [exact])[:1] == ("match",)
    assert diagnostic_verdict(1, 1, [exact, gcc_near], [exact, near])[:1] == ("near",)
    return 0


def main(argv):
    if argv[1:] == ["--self-test"]:
        return run_self_tests()

    prog = argv[0]
    files = []
    mode = False
    verbose = False
    no_extra_credit = False
    args = list(argv[1:])
    index = 0
    while index < len(args):
        arg = args[index]
        if arg == "-v":
            verbose = True
            index += 1
        elif arg == "-my":
            mode = True
            files.extend(collect_my_tests())
            index += 1
        elif arg == "--no-extra-credit":
            no_extra_credit = True
            index += 1
        elif arg == "-third-party":
            if index + 1 >= len(args):
                usage(prog)
                return 2
            chapter = args[index + 1]
            index += 2
            if not chapter.startswith("chapter_"):
                chapter = "chapter_" + chapter
            directory = ""
            if index < len(args) and not args[index].startswith("-"):
                directory = args[index]
                index += 1
            path = f"{TP}/{chapter}" + (f"/{directory}" if directory else "")
            if not os.path.isdir(path):
                sys.stderr.write(f"error: no such test directory: {path}\n")
                return 1
            mode = True
            files.extend(sorted(glob.glob(f"{path}/**/*.c", recursive=True)))
        elif arg in ("-h", "--help"):
            usage(prog)
            return 0
        elif arg.startswith("-"):
            sys.stderr.write(f"error: unknown option '{arg}'\n")
            usage(prog)
            return 2
        else:
            files.append(arg)
            index += 1

    if no_extra_credit:
        files = [path for path in files if "/extra_credit/" not in path]
    if not files and not mode:
        files = collect_my_tests()
    if not files:
        sys.stderr.write("error: no test files found\n")
        return 1

    if shutil.which(CC) is None:
        sys.stderr.write(f"error: C compiler '{CC}' not found\n")
        return 1
    if not os.access(WING, os.X_OK):
        sys.stderr.write(f"error: {WING} not built (run 'make')\n")
        return 1

    colors = pick_colors(verbose and sys.stdout.isatty())
    off, red, grn, yel = colors["off"], colors["red"], colors["grn"], colors["yel"]
    cyn, mag, dim = colors["cyn"], colors["mag"], colors["dim"]

    counts = dict(agree=0, match=0, near=0, differ=0, missed=0, false_pos=0)
    REASON_LABELS = {BODY_MISSING: "body-missing", BODY_EXTRA: "body-extra",
                     BODY_DIFFERS: "body-differs", NO_COUNTERPART: "no-counterpart"}
    reason_names = list(REASON_LABELS.values()) + ["missing-diagnostic"]
    reasons = dict.fromkeys(reason_names, 0)
    for path in files:
        gcc_rc, gcc_out = run([
            CC, "-std=c11", "-pedantic-errors", "-fno-dollars-in-identifiers",
            "-fsyntax-only", path,
        ])
        wing_rc, wing_out = run([WING, path])
        if verbose:
            print(f"{cyn}--- gcc  {path}{off}")
            print(f"{dim}{gcc_out.rstrip()}{off}")
            print(f"{mag}--- wing {path}{off}")
            print(f"{dim}{wing_out.rstrip()}{off}")

        gcc_entries = split_diagnostics(gcc_out)
        wing_entries = split_diagnostics(wing_out)

        verdict, tiers, missing = diagnostic_verdict(
            gcc_rc, wing_rc, gcc_entries, wing_entries)
        if verdict == "agree":
            print(f"{grn}ok      {path}{off}")
            counts["agree"] += 1
        elif verdict == "missed":
            print(f"{cyn}missed  {path} (gcc rejects, wing-gcc accepts){off}")
            counts["missed"] += 1
        elif verdict == "false_pos":
            print(f"{red}FALSE+  {path} (gcc accepts, wing-gcc rejects){off}")
            counts["false_pos"] += 1
        elif verdict == "match":
            print(f"{grn}match   {path}{off}")
            counts["match"] += 1
        elif verdict == "near":
            print(f"{yel}near    {path}{off}")
            counts["near"] += 1
        else:
            labels = []
            for tier in tiers:
                name = REASON_LABELS.get(tier)
                if name and name not in labels:
                    labels.append(name)
            if missing:
                labels.append("missing-diagnostic")
            suffix = f" ({', '.join(labels)})" if labels else ""
            print(f"{yel}differ  {path}{suffix}{off}")
            for name in labels:
                reasons[name] += 1
            counts["differ"] += 1

    print()
    print(f"summary: agree={counts['agree']}  match={counts['match']}  "
          f"near={counts['near']}  differ={counts['differ']}  "
          f"missed={counts['missed']}  false-positives={counts['false_pos']}")
    if counts["differ"]:
        print("differ reasons: " + "  ".join(f"{name}={reasons[name]}" for name in reason_names))
    print("(false-positives = parser coverage debt; 'missed' shrinks as language support grows)")
    return 0 if counts["false_pos"] == 0 else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
