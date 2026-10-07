# Developer tasks for enquber.
#
# The recipes are thin wrappers around cmake and ctest; see CMakePresets.json
# for the configure/build/test presets they use.

# List the available recipes.
default:
    @just --list

# Configure the default build directory.
configure:
    cmake --preset default

[doc('Build the application and the unit tests.')]
build:
    cmake --preset default
    cmake --build --preset default

[doc('Build, then run the unit tests and the fast lints (i18n, ruff, typos).')]
test: build
    ctest --preset default

[doc('Regenerate the translation catalogs (i18n/enquber_*.ts) from the source.')]
i18n-update: configure
    cmake --build --preset default --target update_translations

# Also run as part of `just test`.
[doc('Lint the translations: text IDs, catalog sync and catalog fidelity (Qt 6.11+).')]
lint-i18n:
    cmake --preset default
    ctest --preset default -R '^check_i18n'

# `just test` tolerates unfinished translations so code can land.
[doc('Check that the translated catalogs are complete; the release gate.')]
check-translations:
    cmake --preset default
    ctest --preset release

# Lint everything: C++ static analysis, Python, spelling, translations.
lint: lint-cpp lint-py lint-spell lint-i18n

# Needs the clang build tree (build/lint) and clang-tidy, clazy, run-clang-tidy.
[doc('Static analysis for the C++ sources.')]
lint-cpp:
    cmake --preset lint
    clazy-standalone -p build/lint --only-qt --extra-arg=-Werror src/*.cpp tools/dragsource.cpp
    run-clang-tidy -p build/lint -quiet '/(src|tools)/[^/]+\.cpp$'
    python3 tools/qt_review_lint.py src/*.cpp src/*.h tools/dragsource.cpp

# Lint the Python helpers with ruff.
lint-py:
    ruff check tools tests

# Flag British spellings; the project uses American English.
lint-spell:
    typos

# Arguments go to tools/smoke.py; --help has the flags and an example.
[doc('Build the smoke test helpers, then run the GUI smoke test.')]
smoke-test *args:
    cmake --preset smoke
    cmake --build --preset smoke
    tools/smoke.py {{args}}

# Build, then launch the application.
run: build
    ./build/enquber

# Remove the build directories.
clean:
    rm -rf build

# Create packages
[group('package')]
package-arch: 
    cd packaging/arch && makepkg -f

# Pass --json through tools/startup-stats.py for a table and verdict.
[doc('Record an X11 framebuffer and measure when the UI stops changing.')]
startup-settle *args: build
    tools/startup-settle.py {{args}}

[doc('Compare a candidate binary against a baseline by paired per-round delta.')]
startup-bench baseline candidate *args: build
    tools/startup-settle.py --baseline "{{baseline}}" --candidate "{{candidate}}" {{args}}

