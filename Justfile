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

# Build, then run the unit tests.
test: build
    ctest --preset default

# Build the smoke test helpers, then run the GUI smoke test.
#
# Any extra arguments are passed straight to tools/smoke.py, for example:
#
#     just smoke-test --display :9
smoke-test *args:
    cmake --preset smoke
    cmake --build --preset smoke
    tools/smoke.py --app build/smoke/enquber --dragsource build/smoke/tools/dragsource {{args}}

# Build, then measure how long an app's window takes to stop changing.
#
# This records the framebuffer and compares frames, so it catches late-loading
# UI. Repeat --command to compare applications; the default command is
# ./build/enquber:
#
#     just startup-settle --display :9 --command ./build/enquber --command dolphin
startup-settle *args: build
    tools/startup-settle.py {{args}}

# Defaults for startup-compare; override before the recipe name, e.g.
# `just runs=20 reference=dolphin startup-compare`.
runs := "12"
reference := "kcalc"

# Build, then compare enquber's settled startup against a reference app.
#
# Runs tools/startup-settle.py with the full run count for both ./build/enquber
# and the reference, so the medians are directly comparable. The display comes
# from $DISPLAY (tools/startup-settle.py's own default); pass --display as an
# extra argument to override it. Extra arguments are forwarded verbatim:
#
#     DISPLAY=:9 just startup-compare
#     just runs=20 reference=dolphin startup-compare
#     DISPLAY=:9 just startup-compare --json | tools/startup-stats.py
#
# `runs` and `reference` are justfile variables (just passes recipe parameters
# positionally, so they cannot be given as name=value after the recipe name);
# override them before the recipe.
startup-compare *args: build
    tools/startup-settle.py --runs {{runs}} --command ./build/enquber --command "{{reference}}" {{args}}

# Build, then A/B a candidate binary against a baseline binary with the launch
# order rotated each round, so machine drift cannot favour either side. An
# optional reference (e.g. kcalc) is measured in the same session:
#
#     Xvfb :9 -screen 0 1280x1024x24 -nolisten tcp &
#     DISPLAY=:9 just startup-ab /tmp/enq-main/build/enquber ./build/enquber --reference kcalc
startup-ab baseline candidate *args: build
    tools/startup-ab.py --baseline "{{baseline}}" --candidate "{{candidate}}" {{args}}

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
