# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.22)

# Nested components inside components/hw_init
set(EXTRA_COMPONENT_DIRS
    components/hw_init/spi_init
    components/hw_init/usb_interface
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(RFuzz)
