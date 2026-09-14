cmake_minimum_required(VERSION 3.16)

# Pull in the component under test.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(access_core_host_test)
