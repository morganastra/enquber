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
#     just smoke-test --skip-foreign-drag
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
