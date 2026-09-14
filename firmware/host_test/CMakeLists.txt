cmake_minimum_required(VERSION 3.16)

# Only main and what it REQUIREs. Without this, the linux target tries to
# configure the whole of ESP-IDF — wifi, spi_flash, freertos — none of which
# build on the host, and configuration fails before it reaches our code.
set(COMPONENTS main)
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(access_core_host_test)