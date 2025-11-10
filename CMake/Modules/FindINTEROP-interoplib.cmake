#
# Copyright (c) .NET Foundation and Contributors
# See LICENSE file in the project root for full license information.
#

########################################################################################
# make sure that a valid path is set bellow                                            #
# this is an Interop module so this file should be placed in the CMakes module folder  #
# usually CMake\Modules                                                                #
########################################################################################

# native code directory
set(BASE_PATH_FOR_THIS_MODULE ${PROJECT_SOURCE_DIR}/InteropAssemblies/interoplib)


# set include directories
list(APPEND interoplib_INCLUDE_DIRS ${PROJECT_SOURCE_DIR}/src/CLR/Core)
list(APPEND interoplib_INCLUDE_DIRS ${PROJECT_SOURCE_DIR}/src/CLR/Include)
list(APPEND interoplib_INCLUDE_DIRS ${PROJECT_SOURCE_DIR}/src/HAL/Include)
list(APPEND interoplib_INCLUDE_DIRS ${PROJECT_SOURCE_DIR}/src/PAL/Include)
list(APPEND interoplib_INCLUDE_DIRS ${BASE_PATH_FOR_THIS_MODULE})


# source files
set(interoplib_SRCS

    interoplib.cpp


    interoplib_interoplib_LedPixelController_mshl.cpp
    interoplib_interoplib_LedPixelController.cpp
    interoplib_interoplib_Utilities_mshl.cpp
    interoplib_interoplib_Utilities.cpp

)

foreach(SRC_FILE ${interoplib_SRCS})

    set(interoplib_SRC_FILE SRC_FILE-NOTFOUND)

    find_file(interoplib_SRC_FILE ${SRC_FILE}
        PATHS
	        ${BASE_PATH_FOR_THIS_MODULE}
	        ${TARGET_BASE_LOCATION}
            ${PROJECT_SOURCE_DIR}/src/interoplib

	    CMAKE_FIND_ROOT_PATH_BOTH
    )

    if (BUILD_VERBOSE)
        message("${SRC_FILE} >> ${interoplib_SRC_FILE}")
    endif()

    list(APPEND interoplib_SOURCES ${interoplib_SRC_FILE})

endforeach()

include(FindPackageHandleStandardArgs)

FIND_PACKAGE_HANDLE_STANDARD_ARGS(interoplib DEFAULT_MSG interoplib_INCLUDE_DIRS interoplib_SOURCES)
