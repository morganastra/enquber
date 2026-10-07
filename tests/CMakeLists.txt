# The tests assert on literal strings, so lupdate must not scan them.
set_directory_properties(PROPERTIES QT_EXCLUDE_FROM_TRANSLATION ON)

find_package(Qt6 REQUIRED COMPONENTS Test)

qt_add_executable(tst_enquber tst_enquber.cpp)
target_link_libraries(tst_enquber PRIVATE enquber_core Qt6::Test)
target_compile_options(tst_enquber PRIVATE -Wall -Wextra)

add_test(NAME tst_enquber COMMAND tst_enquber)
# The tests cover widgets and the clipboard, so they need a QApplication but no
# display server.
set_tests_properties(tst_enquber PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
# The save-dialog test drives a modal dialog; a stall has to fail the run
# instead of burning the default ctest timeout.
set_tests_properties(tst_enquber PROPERTIES TIMEOUT 120)

# Run the same binary at a fractional device pixel ratio to exercise the static
# DPR arithmetic (it does not cover a ratio that changes while a widget lives).
add_test(NAME tst_enquber_scaled COMMAND tst_enquber)
set_tests_properties(tst_enquber_scaled PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=1.5")
