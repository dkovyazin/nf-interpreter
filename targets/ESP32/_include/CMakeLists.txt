#
# Copyright (c) .NET Foundation and Contributors
# See LICENSE file in the project root for full license information.
#

# LEDTREES: reset before building the cached list, see targets/ESP32/_common/CMakeLists.txt
set(TARGET_ESP32_IDF_COMMON_INCLUDE_DIRS "")

# append include directory for target 
list(APPEND TARGET_ESP32_IDF_COMMON_INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR})

# make var global
set(TARGET_ESP32_IDF_COMMON_INCLUDE_DIRS ${TARGET_ESP32_IDF_COMMON_INCLUDE_DIRS} CACHE INTERNAL "make global")
