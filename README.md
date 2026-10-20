# wing-gcc

[English](README.md) | [简体中文](README.zh-CN.md)

A C compiler written from scratch in C, built as a personal project to study
how compilers work and to strengthen a binary-security skill set.

## Overview

`wing-gcc` is a self-contained C compiler that translates a practical subset of
C into machine code. The front end is built stage by stage — source handling,
lexical analysis, parsing, and AST construction — and later stages will lower
the AST toward a backend. The goal is to make every layer explicit and
understandable: no parser generator, no external compiler framework, just C and
the operating system.

## Goals

- Implement a real C compiler pipeline end to end, entirely in C.
- Keep the codebase small and readable, targeting roughly **10,000 lines of
  code**, with no third-party C libraries: the compiler links only against
  libc, and the planned backend will drive the system assembler (`as`) and
  linker (`ld`) — as GCC does.
- Serve as a portfolio-quality project for a **binary security / systems
  internship**, demonstrating low-level C, memory management, data structures,
  and language internals.

## Status

| Component         | State                                                        |
| ----------------- | ------------------------------------------------------------ |
| Source management | ✅ `source.c` / `source.h`                                    |
| Lexer             | ✅ Numbers, strings, chars, identifiers, keywords, punctuation, comments |
| AST               | 🚧 Node types and interfaces defined                          |
| Parser            | 🚧 Interfaces in place, `parse()` still a stub                |
| Code generation   | ⬜ Planned                                                    |

The lexer is complete: it turns a source buffer into a linked list of tokens,
handling numeric, string, and character literals (including `u8`/`u`/`U`/`L`
prefixes), identifiers and keywords, punctuators, and line/block comments.
File, line, and column metadata is attached in a separate pass, and lexical
errors are reported GCC-style while lexing continues. The AST node definitions
and parser entry point exist, but parsing logic is still being implemented.

## Architecture

```
        +-----------+     +---------+     +----------+     +-----+
source  |  source.c | --> | lexer.c | --> | parser.c | --> | AST | --> backend
buffer  |           |     | tokenize|     |  parse   |     |     |     (planned)
        +-----------+     +---------+     +----------+     +-----+
                              |                |
                              v                v
                       Token linked list    Node tree
```

The lexer (`tokenize`) produces a `Token` linked list; every token embeds a
`SourceSpan` (file, start, length, line, column). Token generation is kept
separate from metadata attachment, which fills the file and display
line/column in a later pass. The parser consumes the stream and builds a
`Node` tree defined in `ast.h`. Diagnostics are centralized and GCC-style,
built on a shared `DiagKind` + `SourceSpan` core, with `fatal()` aborting on
internal errors. A small open-addressing `HashMap` (`hashmap.c`) backs the
keyword table, and display-width rules live alongside source locations in
`source.h` so token columns and diagnostic carets always agree.

## Directory layout

```
wing-gcc/
├── include/              # Public headers, mirroring the src/ layers
│   ├── util/             # Cross-stage basics
│   │   ├── common.h      # Fatal/alloc utilities
│   │   ├── diagnostic.h  # Error reporting
│   │   ├── hashmap.h     # Hash map
│   │   └── source.h      # SourceFile + SourceSpan
│   ├── frontend/         # Front end
│   │   ├── ast.h         # AST node definitions
│   │   ├── lexer.h       # Lexer interface
│   │   ├── parser.h      # Parser interface
│   │   └── token.h       # Token definitions
│   └── backend/          # Reserved: created with the first backend file
├── src/                  # Compiler implementation, mirroring include/
│   ├── main.c            # Front-end driver
│   ├── util/
│   │   ├── common.c      # Fatal/alloc utilities
│   │   ├── diagnostic.c  # Diagnostics
│   │   ├── hashmap.c     # Hash map
│   │   └── source.c      # Source loading
│   ├── frontend/
│   │   ├── lexer.c       # Token generation
│   │   └── parser.c      # Parser (stub)
│   └── backend/          # Reserved: created with the first backend file
├── tests/            # Test programs
│   ├── tests/        # Internal cases: valid/ accepted, error_lex/ rejected,
│   │                 # warning_lex/ gcc-warns-only; error_parse/ & warning_parse/ reserved
│   └── third_party/writing-a-c-compiler-tests/  # git submodule
├── .clang-format     # C formatting rules
├── .gitmodules
├── test.sh           # gcc differential test harness
└── Makefile
```

## Build

Requires a C compiler (`gcc` or `clang`) and `make`. The compiler itself links
only against libc. Once the backend lands it will drive the system assembler
(`as`) and linker (`ld`), exactly like GCC — those toolchain pieces are used,
not vendored into this project.

```sh
make                # Build the driver into bin/wing-gcc
make check          # Build and run every test driver
make clean          # Remove bin/
```

Individual test drivers: `make lexer_test diagnostic_test source_test hashmap_test`.

### Testing

`make check` builds `wing-gcc` and runs the assertion-based unit tests
(`source_test`, `diagnostic_test`, `hashmap_test`) plus the lexer tests, which
cover token classification, display line/column (tabs, UTF-8), and full
token-stream comparison.

`./test.sh` compares wing-gcc's first diagnostic against real `gcc` on the same
sources and classifies each file as `match`, `differ`, `missed`, or `FALSE+`;
`false-positives` must stay 0. `-my` runs `tests/tests`, `-third-party <chapter>
[dir]` runs the vendored suite, and `-v` prints the full output.

## Usage

In phase 1 the driver only runs the front end: it lexes the file and reports
diagnostics (silent on success, like a compiler front end).

```sh
./bin/wing-gcc tests/tests/basic.c     # lex; silent on success, diagnostics on error
./bin/lexer_test tests/tests/basic.c   # dedicated token-dump driver
```

Example token dump (`lexer_test`):

```
KEYWORD   int
IDENT     main
PUNCT     (
PUNCT     )
PUNCT     {
KEYWORD   return
NUMBER    0
PUNCT     ;
PUNCT     }
EOF
```

## Roadmap

- [x] Source management
- [x] Lexer
- [ ] Parser and AST construction
- [ ] Semantic analysis
- [ ] IR and optimizations
- [ ] x86-64 code generation
- [ ] Self-hosting exploration

## Acknowledgements

The vendored test suite under `tests/third_party/writing-a-c-compiler-tests`
comes from the book *Writing a C Compiler* and is used only for local
correctness checks.
