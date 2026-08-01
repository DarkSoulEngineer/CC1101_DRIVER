# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.22)

# Reuse the project's hw_init components (incl. nested spi_init/usb_interface)
set(EXTRA_COMPONENT_DIRS
    ${CMAKE_CURRENT_LIST_DIR}/../../components/hw_init
    ${CMAKE_CURRENT_LIST_DIR}/../../components/hw_init/spi_init
    ${CMAKE_CURRENT_LIST_DIR}/../../components/hw_init/usb_interface
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(usb_interface_test)
