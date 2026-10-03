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

