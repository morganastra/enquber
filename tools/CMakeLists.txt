# Helpers that only the GUI smoke test needs; not installed.
qt_add_executable(dragsource dragsource.cpp)
target_link_libraries(dragsource PRIVATE qrgen_core Qt6::Widgets)
target_compile_options(dragsource PRIVATE -Wall -Wextra)
