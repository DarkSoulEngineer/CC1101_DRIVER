# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.22)

set(EXTRA_COMPONENT_DIRS
    "components/rfuzz/components/terminal"
    "components/rfuzz/components/profile"
    "components/rfuzz/components/radio"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(RFuzz)