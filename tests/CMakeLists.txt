find_package(Qt6 REQUIRED COMPONENTS Test)

qt_add_executable(tst_qrgen tst_qrgen.cpp)
target_link_libraries(tst_qrgen PRIVATE qrgen_core Qt6::Test)
target_compile_options(tst_qrgen PRIVATE -Wall -Wextra)

add_test(NAME tst_qrgen COMMAND tst_qrgen)
# The tests cover widgets and the clipboard, so they need a QApplication but no
# display server.
set_tests_properties(tst_qrgen PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
