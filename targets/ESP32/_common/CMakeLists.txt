#
# Copyright (c) .NET Foundation and Contributors
# See LICENSE file in the project root for full license information.
#

# LEDTREES: reset the list before building it. It is stored in the CMake cache
# at the bottom of this file (CACHE INTERNAL), so on a reconfigure list(APPEND)
# would start from the previous run's value: entries pile up duplicated and a
# source removed from this list stays in the cache forever. That is how the
# build broke with "Cannot find source file .../targetHAL_StorageOperation.cpp"
# after upstream PR #3502 deleted it - the file was gone from here, but not from
# the cache. Same reset in _include, _nanoCLR and _Network.
set(TARGET_ESP32_IDF_COMMON_SOURCES "")

# append target common source files 
list(APPEND TARGET_ESP32_IDF_COMMON_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/Device_BlockStorage.c)
list(APPEND TARGET_ESP32_IDF_COMMON_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/GenericPort_Write.cpp)
list(APPEND TARGET_ESP32_IDF_COMMON_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/platform_BlockStorage.c)
list(APPEND TARGET_ESP32_IDF_COMMON_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/platform_heap.c)
list(APPEND TARGET_ESP32_IDF_COMMON_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/Target_BlockStorage_ESP32FlashDriver.c)
list(APPEND TARGET_ESP32_IDF_COMMON_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/targetHAL.c)
list(APPEND TARGET_ESP32_IDF_COMMON_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/targetHAL_Time.cpp)
list(APPEND TARGET_ESP32_IDF_COMMON_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/targetHAL_Rtos.c)
list(APPEND TARGET_ESP32_IDF_COMMON_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/WireProtocol_HAL_Interface.c)
list(APPEND TARGET_ESP32_IDF_COMMON_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/WireProtocol_ReceiverThread.c)

list(APPEND TARGET_ESP32_IDF_COMMON_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/targetHAL_ConfigStorageLittlefs.c)
list(APPEND TARGET_ESP32_IDF_COMMON_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/targetHAL_ConfigurationManager.cpp)
list(APPEND TARGET_ESP32_IDF_COMMON_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/targetHAL_FileOperation.cpp)
list(APPEND TARGET_ESP32_IDF_COMMON_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/nanoSupport_CRC32.c)
list(APPEND TARGET_ESP32_IDF_COMMON_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/DeviceMapping_common.cpp)

if(NF_FEATURE_OTA)
    list(APPEND TARGET_ESP32_IDF_COMMON_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/targetHAL_Ota.c)
endif()

# add device mapping
# check if there is a specific one for the target...
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${TARGET_NAME}_DeviceMapping.cpp")
    list(APPEND TARGET_ESP32_IDF_COMMON_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/${TARGET_NAME}_DeviceMapping.cpp)
else()
    # go with the default one for the series
    list(APPEND TARGET_ESP32_IDF_COMMON_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/${TARGET_SERIES}_DeviceMapping.cpp)
endif()


# make var global
set(TARGET_ESP32_IDF_COMMON_SOURCES ${TARGET_ESP32_IDF_COMMON_SOURCES} CACHE INTERNAL "make global")
