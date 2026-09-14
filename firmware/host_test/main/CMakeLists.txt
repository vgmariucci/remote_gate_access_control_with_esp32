idf_component_register(
    SRCS         "host_test_main.c"
    INCLUDE_DIRS "."
    REQUIRES     unity access_core
    WHOLE_ARCHIVE
)

# Compile the shared test file into this app so that device runs and host
# runs execute exactly the same assertions.
target_sources(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../components/access_core/test/test_access_core.c"
)
