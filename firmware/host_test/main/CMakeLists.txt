idf_component_register(
    SRCS         "host_test_main.c"
    INCLUDE_DIRS "."
    REQUIRES     unity access_core ui keypad
    WHOLE_ARCHIVE
)

# Shared test files, compiled into this app so host runs and device runs
# execute exactly the same assertions.
target_sources(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../components/access_core/test/test_access_core.c"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/ui/test/test_ui.c"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/keypad/test/test_keypad_logic.c"
)