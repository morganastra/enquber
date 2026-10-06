# Linting enquber: options and a recommended setup

Proposal, October 2026. All counts were measured on this worktree with Qt 6.11.2,
clang-tidy/clang 23.1.1, clazy 1.17.1, ruff 0.16.10, typos 1.50.3 (Arch Linux).
Counts drift as the tree changes; the commands are included so they can be re-measured.

Status: the recommended stack was adopted. Section 10 records the decisions and
where each piece landed.

## 1. Where the project stands

| Area   | Today |
|--------|-------|
| C++    | `-Wall -Wextra` via `target_compile_options`; Qt `auto` tests; no clang-tidy or clazy config |
| Python | no config, no linter; 8 scripts under `tools/` (~3 000 lines) |
| i18n   | hand-rolled `tools/check-i18n.py` (421 lines) registered as the `check_i18n` ctest |
| Text   | no spelling check; the tree is deliberately American English with a few strays (`licence`, `synthesised`, `Summarise`) |

Constraints to respect:

* Qt 6.5 is the documented minimum (README), so newer Qt checks must degrade gracefully.
* No CI; `just` is the front door, CMake/ctest is the engine.
* macOS must keep working; tools should be optional and PATH-guarded.
* On Arch, `/usr/bin/lupdate` is **Qt 5.15** (qt5-tools) and shadows the Qt 6 tools in
  `/usr/lib/qt6/bin`. Always reach Qt 6 tools through CMake targets
  (`Qt6::lupdate`, `Qt6::lrelease`, `Qt6::lcheck`) or an absolute Qt 6 path, never bare `lupdate`.

## 2. Recommended stack at a glance

| Job | Tool | Why |
|-----|------|-----|
| Qt-specific C++ | clazy `level1` | 4 real findings on the current tree, zero config, Qt idioms |
| Generic C++ | clang-tidy, curated `.clang-tidy` | analyzer + bugprone + selected performance/modernize; 4 findings |
| Qt review rules | Qt's `qt_review_lint.py` (vendored, BSD-3) | 60 deterministic Qt review rules, no compile DB needed; 6 findings |
| Python | ruff, explicit `select` in `ruff.toml` | 26 findings, 10 auto-fixable; fast, single binary |
| Spelling | typos with `locale = "en-us"` | flags British variants in comments **and** identifiers; 31 findings; single binary |
| i18n | Qt `lcheck` (>= 6.11) + `lrelease -fail-on-unfinished` (>= 6.10), plus a slimmed `check-i18n.py` | removes ~200 lines of hand-written catalog validation; the rest has no off-the-shelf equivalent |
| Agent review | Qt `qt-cpp-review` skill under `.opencode/skills/` | deterministic linter + six deep-analysis prompts; read-only |

Optional: cppcheck as a third C++ opinion, `ruff format` for Python formatting,
and a `.clang-format` for C++ formatting.

## 3. C++ options

### 3.1 clazy (recommended)

clazy is a Clang plugin with ~70 Qt-specific checks; `level1` is the sensible default.
Run on the clang compile database:

```sh
clazy-standalone -p build/lint --only-qt src/*.cpp
```

Measured on the current tree (11 files, level1):

```
src/mainwindow.cpp:73   non-POD static (QString kPlaceholderText)        [-Wclazy-non-pod-global-static]
src/mainwindow.cpp:995  temporary QRegularExpression                     [-Wclazy-use-static-qregularexpression]
src/mainwindow.cpp:996  temporary QRegularExpression                     [-Wclazy-use-static-qregularexpression]
src/mimetext.cpp:15     C++11 range-loop might detach Qt container (QList) [-Wclazy-range-loop-detach]
```

Gotcha: a GCC-configured `compile_commands.json` contains `-mno-direct-extern-access`,
which clazy's Clang does not know (`error: unknown argument`). Point clazy/clang-tidy at
a clang-configured tree (a small `lint` CMake preset with `CMAKE_CXX_COMPILER=clang++`)
or strip that flag from a copy of the database. On macOS clang is already the default.

`level2` is available as a later, more opinionated step; it is noticeably noisier and
should not be the starting point.

### 3.2 clang-tidy (recommended, curated)

Run:

```sh
run-clang-tidy -p build/lint -quiet
```

Measured with `clang-analyzer-*,bugprone-*`: 2 findings
(`mainwindow.cpp:73` throwing static initialization, `qrcode.cpp:86` signed bitwise —
the latter is likely benign, the result of integer promotion).

Measured with `performance-*,modernize-*`:

```
67  modernize-use-trailing-return-type   <- style preference, disable
16  modernize-use-nodiscard
 2  performance-use-std-move
 2  performance-enum-size
 2  modernize-return-braced-init-list
 1  performance-no-automatic-move
```

Proposed `.clang-tidy` to start from (explicit, so clang-tidy upgrades do not silently
change the rule set):

```yaml
Checks: >
  clang-analyzer-*,
  bugprone-*,
  performance-*,
  modernize-use-nodiscard,
  modernize-use-nullptr,
  modernize-use-override,
  modernize-use-emplace,
  modernize-return-braced-init-list
WarningsAsErrors: ""
HeaderFilterRegex: '^(src|tools)/'
FormatStyle: none
```

Qt Creator reads `.clang-tidy` automatically, so editors get the same rules.

### 3.3 Qt's own review linter (recommended)

The `qt-cpp-review` skill published by The Qt Company bundles a deterministic,
stdlib-only Python linter with 60 rules across include order, deprecated classes,
anti-patterns, model contracts, error handling, lifecycle, naming, enums, headers,
timeouts, and ternaries. License is BSD-3-Clause, so the linter is vendored
standalone at `tools/qt_review_lint.py` (with the license text alongside); the
agent skill itself was not adopted.

```sh
python3 tools/qt_review_lint.py src/*.cpp src/*.h tools/dragsource.cpp
```

Measured on the current tree, 6 findings:

```
src/mainwindow.cpp:64   TMO-1  integer timeout/interval, prefer QDeadlineTimer/chrono
src/mainwindow.cpp:958  HDR-3  unprotected std::min/max (Windows min/max macros)
src/mainwindow.h:65     ENM-2  unscoped enum without explicit underlying type
src/mimeimage.cpp:14    ERR-1  QBuffer::open() return not checked
src/qrview.cpp:75       HDR-3  unprotected std::min/max
src/qrview.cpp:76       HDR-3  unprotected std::min/max
```

It needs no compile database and runs in well under a second, so it can live in the
same fast lint pass as ruff and typos.

### 3.4 cppcheck (optional, not measured here)

`cppcheck` (Arch: `cppcheck`, Homebrew: `cppcheck`) adds independent value-flow
analysis and a Qt library model:

```sh
cppcheck --project=build/compile_commands.json --enable=warning,performance,portability \
         --library=qt --inline-suppr --suppress=missingIncludeSystem
```

It is a second opinion with its own false positives; add it only if clazy + clang-tidy
turn out to miss things. It is not installed on this machine, so there is no measured
baseline for it.

### 3.5 C++ recommendation

clazy `level1` + curated clang-tidy + the Qt review linter. Each finds things the others
miss; together they cost one `just lint-cpp` run. Keep clang-tidy/clazy out of ctest —
they are too slow for every `just test`.

## 4. Python: ruff

Single decision: ruff with an explicitly pinned rule set. Measured on `tools/` + `tests/`:

* `--select F` (classic minimum): 1 finding.
* Recommended set `E4,E7,E9,F,I,UP,B,SIM,RUF`: ~18 findings, much of it auto-fixable
  (unused `noqa`, unsorted imports, f-string without placeholders, bare `except`,
  `subprocess.run` without `check=`).
* Adding `PTH` (`os.path` -> `pathlib`): +8 findings, noisier churn; defer.

Proposed `ruff.toml`:

```toml
target-version = "py310"
line-length = 100

[lint]
# Kept explicit: ruff's stable default set grows between releases.
select = ["E4", "E7", "E9", "F", "I", "UP", "B", "SIM", "RUF"]
```

Then a one-time `ruff check --fix tools tests` and a small hand-fix pass for the rest.
`ruff format --check` is available later but would reformat ~3 000 lines; treat it as a
separate decision. A type checker (mypy/pyright) is not a linter; the scripts are
annotated, so it could be added later, but PyQt6/Xlib stubs make it a project of its own.

## 5. Spelling: typos with `locale = "en-us"`

The project uses American English, so the rule is to flag British variants. `typos`
does exactly this with a locale switch, understands comments, strings *and* identifiers,
and is a fast single binary (Arch: `typos`, Homebrew: `typos-cli`). It also respects
`.gitignore`.

Measured on the current tree with the config below: 31 findings, essentially all real:

```
23  licence  -> license    (src/aboutpage.{h,cpp}, tests/tst_enquber.cpp)
 5  Licence  -> License    (same files)
 1  synthesised -> synthesized  (tools/dragsource.cpp)
 1  Summarise   -> Summarize    (tools/startup-stats.py)
 1  summarised  -> summarized   (tools/startup-settle.py)
```

Proposed `typos.toml`:

```toml
[files]
# Translations are not English; the //% source text is checked in src/.
extend-exclude = ["i18n/*.ts"]

[default]
locale = "en-us"

[default.extend-words]
# "PNGs" is tokenised as PN + Gs; PN is not a typo for ON.
PN = "PN"
```

`typos --write-changes` can land the mechanical fixes, but review the diff: renaming the
`about.licence` text ID also needs `just i18n-update`, and the stale catalog entry has to
go (same as any ID rename today).

Alternatives considered:

| Tool | Verdict |
|------|---------|
| typos (`en-us`) | **Chosen.** Locale support, identifier-aware, one binary, Prettier-style ignore comments. |
| codespell + custom dictionary | Works (verified `color->colour` mappings) but only catches standalone lowercase words, misses `QColor`-style identifiers, ships a questionable default correction (`requestor -> requester`, wrong for X11), and needs Python. |
| cspell (`flagWords`) | Powerful but needs Node; more config for the same result on a small tree. |
| Vale / Harper | Prose-oriented; source-code comment coverage is weaker than typos. |

## 6. i18n: replacing `tools/check-i18n.py`

Qt now owns most of what the script reimplements. Mapping:

| `check-i18n.py` check | Off-the-shelf replacement | Available from |
|---|---|---|
| catalog placeholders, accelerator, surrounding whitespace, final punctuation | `lcheck --check-finished i18n/enquber_*.ts` | Qt **6.11** (`Qt6::lcheck` target exists; verified: passes both catalogs, and fails a planted `%2` mismatch) |
| `--require-translated es` completeness | `lrelease -fail-on-unfinished i18n/enquber_es.ts`, behind `just check-translations` | Qt **6.10** (verified: es passes, en fails — the English catalogue is intentionally `unfinished`) |
| missing `//%` comment / empty `<source>` | `lupdate` warns `Message with id '...' has no source.`; with `-warnings-are-errors` it prints `lupdate error: ...` | Qt **6.10** for the flag |
| source <-> catalog ID sync | keep the script's small set-diff, or compare a `lupdate -locations none` temp catalog against the committed one | Qt 6.5+ |
| no `tr()`/`translate()`/`QT_TR*_NOOP` | **no off-the-shelf equivalent** | — |
| user-facing literal passed to a widget | **no off-the-shelf equivalent** (heuristic, small allowlist) | — |

Two sharp edges found while testing:

1. **`lupdate -warnings-are-errors` does not fail the process for a missing source.**
   It relabels the warning (`lupdate error: Message with id 'x.nope' has no source.`)
   but exits 0; upstream `projectprocessor.cpp` does `if (options & Werror) return !fail;`
   which returns success when that is the only problem. So the gate must either keep the
   script's `//%` check or parse lupdate's output for `lupdate error:`.
2. **Version floor.** The project supports Qt 6.5, but `lcheck` is 6.11+ and the two
   other flags are 6.10+. The CMake side can check exactly:
   `if(TARGET Qt6::lcheck)`, `if(Qt6_VERSION VERSION_GREATER_EQUAL 6.10)`.

Recommended shape:

* Keep `check-i18n.py`, but slim it to the two source-side heuristics above plus the ID
  sync diff; delete its catalog fidelity and completeness code.
* Register two extra ctests, guarded so old Qt still works:

  ```cmake
  if(Qt6_VERSION VERSION_GREATER_EQUAL 6.10 AND TARGET Qt6::lrelease)
      add_test(NAME check_i18n_es COMMAND $<TARGET_FILE:Qt6::lrelease>
               -fail-on-unfinished ${CMAKE_CURRENT_SOURCE_DIR}/i18n/enquber_es.ts
               -qm ${CMAKE_BINARY_DIR}/lint-es.qm)
  endif()
  if(TARGET Qt6::lcheck)
      add_test(NAME check_i18n_lcheck COMMAND $<TARGET_FILE:Qt6::lcheck>
               --check-finished ${CMAKE_CURRENT_SOURCE_DIR}/i18n/enquber_en.ts
               ${CMAKE_CURRENT_SOURCE_DIR}/i18n/enquber_es.ts)
  endif()
  ```

  Using the CMake targets also sidesteps the Qt 5 `lupdate` shadowing.

Alternatives: Translate Toolkit (`ts2po` + `pofilter`) can validate PO files, but it is an
extra Python dependency, does a lossy round-trip, and still cannot see `//%` conventions
— not worth it. Keeping the whole script is the zero-risk fallback; only the catalog
half is duplicated by Qt tooling.

## 7. Agent skills from Qt

Not adopted. The skills at <https://github.com/TheQtCompanyRnD/agent-skills>
(commit `4b33744`) were considered; the `qt-cpp-review` skill's deterministic
linter is vendored standalone as `tools/qt_review_lint.py` (BSD-3-Clause,
license text kept next to it), while the six deep-analysis agent prompts and the
OpenCode skill packaging were skipped to keep the repository tool-agnostic.

## 8. Integration

Implemented in the Justfile:

```just
# Lint everything: C++ static analysis, Python, spelling, translations.
lint: lint-cpp lint-py lint-spell lint-i18n

lint-cpp:
    cmake --preset lint
    clazy-standalone -p build/lint --only-qt --extra-arg=-Werror src/*.cpp tools/dragsource.cpp
    run-clang-tidy -p build/lint -quiet '/(src|tools)/[^/]+\.cpp$'
    python3 tools/qt_review_lint.py src/*.cpp src/*.h tools/dragsource.cpp

lint-py:
    ruff check tools tests

lint-spell:
    typos

# Translations: text IDs, catalog sync, and lcheck where available.
lint-i18n:
    cmake --preset default
    ctest --preset default -R '^check_i18n'

# Release gate: every translated catalog is complete.
check-translations:
    cmake --preset default
    ctest --preset release
```

Supporting changes:

* The `lint` configure preset (`binaryDir: build/lint`, `ENQUBER_BUILD_TESTS=OFF`,
  `ENQUBER_BUILD_TEST_TOOLS=ON`, `CMAKE_CXX_COMPILER=clang++`,
  `CMAKE_EXPORT_COMPILE_COMMANDS=ON`). The clang build avoids the GCC-only
  `-mno-direct-extern-access` flag, which clazy/clang-tidy reject.
* The fast checks are registered as guarded ctests: `check_i18n` always (Python),
  `check_i18n_lcheck_*` when `Qt6::lcheck` exists (Qt 6.11+), and
  `check_i18n_translated_es` on Qt 6.10+; `ruff` and `typos` tests are added with
  `find_program` when installed. The completeness test is labeled `release`,
  excluded by the default test preset and run by `just check-translations`.
* No git hook. The entry points are `just lint` (everything) and `just
  lint-i18n` / `lint-cpp` / `lint-py` / `lint-spell` for one part. A
  catalog-refreshing pre-commit hook was tried and dropped: it needed a
  partial-staging guard, and the pre-commit framework's pinned per-hook
  environments and 127 MB cache were not worth it for two system tools.
* `.ruff_cache/` is in `.gitignore`.
* Optional follow-ups (separate decisions): `.clang-format` (Qt/WebKit style, 4 spaces),
  `ruff format`, `cppcheck`, `mypy`.

## 9. Initial cleanup to go green

| Area | Findings |
|------|----------|
| clazy | 4 (two QRegularExpression statics, one non-POD global, one range-loop detach) |
| clang-tidy | 4 (2 analyzer/bugprone, 2 performance; `modernize-use-nodiscard` is 16 mechanical additions) |
| Qt review linter | 6 (TMO-1, ENM-2, ERR-1, 3x HDR-3) |
| ruff | ~18 (about 10 auto-fixable; `subprocess.run(check=...)` needs judgement) |
| typos | 31 (28x `licence`, 3x other) |
| i18n | refactor of `check-i18n.py` (delete catalog checks, keep source checks) |

None of these are large; the whole cleanup is likely one focused sitting.

## 10. Decisions taken

1. C++: clazy `level1` + curated clang-tidy + the Qt review linter (vendored at
   `tools/qt_review_lint.py`), implemented via `.clang-tidy`, the `lint` CMake
   preset and `just lint-cpp`.
2. Python: ruff with the explicit `select` set above; the tree is clean.
3. Spelling: typos with American English (flags British variants); all 31
   findings were fixed, including `about.licence` -> `about.license`.
4. i18n: `check-i18n.py` slimmed to source conventions + ID sync; `lcheck`
   (Qt 6.11+) runs in `just test`; completeness
   (`lrelease -fail-on-unfinished`, Qt 6.10+) is the `release`-labeled test
   behind `just check-translations`, so unfinished Spanish does not block
   development. Catalogs are refreshed explicitly with `just i18n-update`, and
   `qt_add_translations` passes `-no-obsolete`.
5. Agent skill: not vendored; only its deterministic linter, with the BSD-3
   license text.
6. Enforcement: `just lint` is the full gate (`lint-cpp`, `lint-i18n`,
   `lint-py`, `lint-spell`), and `just test` runs the fast checks where
   installed. No git hooks: the hook that refreshed catalogs needed a
   partial-staging guard and was dropped as too much machinery for this repo.
7. Formatting (`.clang-format`, `ruff format`) deferred.
