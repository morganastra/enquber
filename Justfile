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

# Build the application and the unit tests.
build:
    cmake --preset default
    cmake --build --preset default

# Build, then run the unit tests plus the translation-catalog lint.
test: build
    ctest --preset default

# Regenerate the translation catalogs (i18n/enquber_*.ts) from the source.
i18n-update: configure
    cmake --build --preset default --target update_translations

# Lint the translations: text IDs, catalog sync, and (Qt 6.11+) catalog
# fidelity. Also run as part of `just test`.
lint-i18n:
    cmake --preset default
    ctest --preset default -R '^check_i18n'

# Check that the translated catalogs are complete. This is the release gate;
# `just test` deliberately tolerates unfinished translations so code can land
# before the Spanish text does.
check-translations:
    cmake --preset default
    ctest --preset release

# Lint everything: C++ static analysis, Python, spelling, translations.
lint: lint-cpp lint-py lint-spell lint-i18n

# Static analysis for the C++ sources. Configures a clang build tree with a
# compile database (build/lint); needs clang-tidy, clazy and run-clang-tidy.
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

# Build the smoke test helpers, then run the GUI smoke test.
#
# Extra arguments are passed to tools/smoke.py. For example to run
# on the real display:
#
#     just smoke-test --no-xvfb
smoke-test *args:
    cmake --preset smoke
    cmake --build --preset smoke
    tools/smoke.py --app build/smoke/enquber --dragsource build/smoke/tools/dragsource {{args}}

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

# Shortcuts for startup performance measurement:
#
# startup-settle rotates the launch order every round, so its medians are
# drift-resistant; pass --json and pipe through tools/startup-stats.py for a
# table and verdict. startup-bench compares a candidate binary against a
# baseline with the paired per-round delta:
#
#     DISPLAY=:9 just startup-settle --runs 5 --command ./build/enquber --command kcalc
#     DISPLAY=:9 just startup-bench /tmp/enq-main/build/enquber ./build/enquber --runs 15
startup-settle *args: build
    tools/startup-settle.py {{args}}

startup-bench baseline candidate *args: build
    tools/startup-settle.py --baseline "{{baseline}}" --candidate "{{candidate}}" {{args}}

