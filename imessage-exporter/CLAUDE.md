# CLAUDE.md — imessage-exporter

Context for Claude Code (and humans) working in this repo. Read this first.

## What this is

A small, fast **C++17** command-line tool that exports macOS iMessage / SMS
history to portable formats (**TXT**, **JSON**, **HTML**). It reads the local
Messages SQLite database (`~/Library/Messages/chat.db`) **read-only** and links
the system `libsqlite3` directly — a lean native binary with no third-party
runtime dependencies.

- **Language:** C++17, CMake build.
- **License:** MIT (see `LICENSE`).
- **Status:** v0.1.0. Builds, all tests pass, verified end-to-end against a
  synthetic `chat.db`. Not yet run against a real macOS database — see
  "Known gaps".

### Origin / provenance

This is an **independent, clean-room C++ reimplementation** inspired by the Rust
project [ReagentX/imessage-exporter](https://github.com/ReagentX/imessage-exporter).
It shares **no code** with it. We chose C++ deliberately ("close to the metal":
native binary, direct SQLite C API, no interpreter) and MIT licensing
specifically to avoid the reference project's GPL-3.0.

This repo (`grioghar/imessage-exporter-redux`) was extracted from a subdirectory
of `grioghar/macos-rdp-server`, where it was originally developed on branch
`claude/imessage-exporter-library-t6NVE` (PR #2). The history was split out with
`git subtree split` so the project sits at the repo root here. The original
macos-rdp-server repo is an unrelated C/FreeRDP project — none of its code is here.

## Layout

```
.
├── include/imsg/        # public headers
│   ├── models.hpp       # Chat / Message / Attachment structs
│   ├── time_util.hpp    # Apple "Mac absolute time" conversion
│   ├── attributed_body.hpp
│   ├── exporters.hpp    # txt / json / html renderers
│   └── database.hpp     # read-only chat.db reader (SQLite)
├── src/                 # one .cpp per header, + main.cpp (CLI)
├── tests/test_core.cpp  # dependency-free unit tests (33 assertions)
├── docs/SCHEMA.md       # database schema notes & quirks — READ THIS
├── CMakeLists.txt
├── README.md
└── LICENSE              # MIT
```

### Architecture: the core/SQLite split (important)

Tricky logic lives in a **SQLite-free core library** (`imsg_core`):
`models`, `time_util`, `attributed_body`, `exporters`. Only `database.cpp` and
`main.cpp` depend on SQLite. This is intentional — it lets the parsing/formatting
logic be unit-tested on **any** platform, even without `libsqlite3` headers.

CMake reflects this:
- `imsg_core` (static lib) + `imsg_tests` always build (no SQLite needed).
- The `imessage-exporter` binary only builds when `find_package(SQLite3)`
  succeeds. On macOS SQLite3 ships with the system; on Linux install
  `libsqlite3-dev`. If SQLite isn't found, CMake prints a warning and builds
  core + tests only.

**When adding logic, prefer putting it in the SQLite-free core so it stays
testable.** Don't pull SQLite into `exporters`/`models`/`time_util`.

## Build / test / run

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build          # runs core unit tests
```

Run (binary at `build/imessage-exporter`):

```bash
./build/imessage-exporter --format html --output ./export       # default DB
./build/imessage-exporter --db ./chat.db --format json --output ./export
./build/imessage-exporter --list-chats
```

### CLI options

| Flag | Description | Default |
| --- | --- | --- |
| `--db PATH` | Path to the Messages database. | `$HOME/Library/Messages/chat.db` |
| `--format FMT` | `txt`, `json`, or `html`. | `txt` |
| `--output DIR` | Output directory (one file per conversation). | `./imessage-export` |
| `--me NAME` | Label for messages you sent. | `Me` |
| `--list-chats` | List conversations and exit. | — |
| `--version` / `--help` | Print and exit. | — |

On modern macOS the terminal needs **Full Disk Access** to read `chat.db`.

## The two quirks that matter (details in docs/SCHEMA.md)

1. **Timestamps** — `message.date` is an offset from **2001-01-01 UTC**, in
   **nanoseconds** on macOS 10.13+ and **seconds** on older DBs.
   `apple_time_to_epoch` (time_util) auto-detects by magnitude
   (threshold 1e11) and converts to Unix epoch seconds.

2. **`attributedBody`** — on modern macOS the plain `message.text` column is
   often `NULL` and the visible text lives in the `attributedBody` BLOB: an
   `NSAttributedString` serialized with the legacy NeXTSTEP **typedstream**
   (`streamtyped`) archiver. `decode_attributed_body` (attributed_body) extracts
   the length-prefixed UTF-8 payload after the `NSString`/`NSMutableString`
   class marker (a `0x81` length byte ⇒ following little-endian uint16 length),
   validates UTF-8, rejects control-char noise, and returns `""` on unexpected
   layouts so export never crashes. **This decoder is heuristic** — the most
   likely place to need hardening against a real-world `chat.db`.

The reader also adapts to schema differences across macOS versions via
`PRAGMA table_info(...)`, falling back to `NULL` for absent columns
(`attributedBody`, `chat.service_name`, `message.service`).

## Conventions

- C++17, `-Wall -Wextra` clean. Match the existing terse style.
- Comments explain *why*, not *what*; keep density similar to surrounding code.
- The DB is opened `mode=ro&immutable=1` — **never** add write paths to the
  live database.
- Tests are a tiny hand-rolled harness in `tests/test_core.cpp` (no framework);
  add assertions there. Keep them SQLite-free.

## Known gaps / good next tasks

- ⚠️ **Verify against a real macOS `chat.db`.** Only synthetic data has been
  exercised so far; the `attributedBody` decoder is the likely weak point.
- Attachment **file copying** (currently only metadata is exported).
- **Date-range** filtering (`--since` / `--until`).
- **Combined** single-file export option (vs one file per chat).
- **Contact-name resolution** from the macOS Contacts (AddressBook) DB so
  senders show names instead of phone numbers/emails.
- A CI workflow (`.github/workflows/ci.yml`: cmake build + ctest) — not yet added.

## Git

- Default branch in this repo: `main`.
- The original development PR (for reference/history) is
  `grioghar/macos-rdp-server` PR #2.
