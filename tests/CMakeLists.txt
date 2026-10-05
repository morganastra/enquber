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
