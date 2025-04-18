cmake_minimum_required(VERSION 3.31)
project(
  gps
  VERSION 0.1.0
  DESCRIPTION "GPU utils"
  HOMEPAGE_URL "https://github.com/changhaoxuan23/gps"
  LANGUAGES C CXX
)

# export compile_commands.json to help with development
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

##########################
### tune compile flags ###
##########################

# c++23 is required since we use std::format and std::print*
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# check for headers since a compile having partial support to print and format
#  can still be common for now
include(CheckIncludeFileCXX)
check_include_file_cxx(format CXX_FORMAT_AVAILABLE)
check_include_file_cxx(print CXX_PRINT_AVAILABLE)

if(NOT CXX_FORMAT_AVAILABLE OR NOT CXX_PRINT_AVAILABLE)
  message(FATAL_ERROR "compiler ${CMAKE_CXX_COMPILER} does not support format or print")
endif()

# set compile options based on build type
set(CMAKE_CXX_FLAGS_DEBUG -g3)
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "-g3 -O2 -DNDEBUG")

# make default build type to Release
if(NOT DEFINED CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# check for lto support
include(CheckIPOSupported)
check_ipo_supported(RESULT LTO_AVAILABLE)

#####################
### setup options ###
#####################

# main options
option(GPS_ENABLE_GPS     "Build gps, ps for GPUs"                                          ON)
option(GPS_ENABLE_GLAUNCH "Build glaunch, launching helper for programs that utilizes GPUs" ON)
option(GPS_ENABLE_GTRACE  "Build gtrace, GPU API (CUDA API) call tracer"                    ON)

# side options
option(GPS_ENABLE_HOOKED_API_BUILDER "Build build of hooked APIs" OFF)

########################
### prepare packages ###
########################

# commandline argument parser
find_package(ConfigurationsPP REQUIRED)

# threading support
find_package(Threads REQUIRED)

# CUDA toolkit
find_package(CUDAToolkit REQUIRED)

# python C API, which is used to generate python traceback when applicable
find_package(Python QUIET COMPONENTS Development)

# pkgconf to find other libraries
find_package(PkgConfig REQUIRED)

# keyutils, library to access the kernel keyring
pkg_search_module(keyutils QUIET IMPORTED_TARGET libkeyutils)

#########################
### configure targets ###
#########################

# utils, now only used in glaunch and gps
if(GPS_ENABLE_GPS OR GPS_ENABLE_GLAUNCH)
  add_library(gps-utils STATIC utils.cc)
  target_include_directories(gps-utils PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
endif()

# nvml_common, wrapper around NVML to provide simplified query interface
if(GPS_ENABLE_GPS OR GPS_ENABLE_GLAUNCH)
  # this internal library is only required by gps and glaunch
  add_library(nvml_common STATIC nvml_common.cc)
  target_include_directories(nvml_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(nvml_common PUBLIC CUDA::nvml)
endif()

# internal library: hooked API
# this is only used in glaunch and gtrace
if(GPS_ENABLE_GLAUNCH OR GPS_ENABLE_GTRACE)
  add_subdirectory(tools/hooked_api_builder)
endif()

# gps, the process lister and inspector for GPU
if(GPS_ENABLE_GPS)
  add_executable(gps gps.cc)
  target_compile_definitions(gps PRIVATE GPS_VERSION="${PROJECT_VERSION}")
  target_link_libraries(gps PRIVATE nvml_common gps-utils)

  # subdirectory: gps-internals
  add_subdirectory(gps-internal)
endif()

# glaunch, the program launching helper
if(GPS_ENABLE_GLAUNCH)
  add_executable(glaunch glaunch.cc)
  target_compile_definitions(glaunch PRIVATE GPS_VERSION="${PROJECT_VERSION}")
  target_include_directories(glaunch PRIVATE ${CMAKE_CURRENT_LIST_DIR})

  # subdirectory: glaunch modules
  add_subdirectory(glaunch-modules)

  target_link_libraries(
    glaunch PRIVATE
    ConfigurationsPP
    nvml_common
    Threads::Threads
    hooked_api_config
    gps-utils
  )
endif()


if(GPS_ENABLE_GTRACE)
  # build gtrace: GPU call tracer
  add_executable(gtrace gtrace.cc)
  target_compile_definitions(gtrace PRIVATE GPS_VERSION="${PROJECT_VERSION}")
  target_link_libraries(gtrace PRIVATE hooked_api_config PRIVATE ConfigurationsPP)
endif()

##################################
### configuration installation ###
##################################

if(GPS_ENABLE_GPS)
  install(TARGETS gps)
endif()

if(GPS_ENABLE_GLAUNCH OR GPS_ENABLE_GTRACE)
  install(TARGETS hooked_api)
endif()

if(GPS_ENABLE_GLAUNCH)
  install(TARGETS glaunch)
endif()

if(GPS_ENABLE_GTRACE)
  install(TARGETS gtrace)
endif()

##########################
### generate a summary ###
##########################

function(prepare_bool_for_print variable)
  if(${variable})
    set(${variable} "yes" PARENT_SCOPE)
  else()
  set(${variable} "no" PARENT_SCOPE)
  endif()
endfunction()
prepare_bool_for_print(GPS_ENABLE_GPS)
prepare_bool_for_print(GPS_ENABLE_GLAUNCH)
prepare_bool_for_print(GPS_EMAIL_NOTIFIER_ENABLED)
prepare_bool_for_print(GPS_ENABLE_GTRACE)
prepare_bool_for_print(Python_FOUND)

message("========================== configuration summary =========================")
message("  Build type                   : ${CMAKE_BUILD_TYPE}                      ")
message("  Building with CUDA           : ${CUDAToolkit_VERSION}                   ")
message("  Building gps                 : ${GPS_ENABLE_GPS}                        ")
message("  Building glaunch             : ${GPS_ENABLE_GLAUNCH}                    ")
message("    Email notifier enabled     : ${GPS_EMAIL_NOTIFIER_ENABLED}            ")
message("  Building gtrace              : ${GPS_ENABLE_GTRACE}                     ")
message("    Python stacktrace enabled  : ${Python_FOUND}                          ")

if(Python_FOUND)
message("    Build with python          : ${Python_VERSION}                        ")
endif()

message("==========================================================================")

set(CPACK_SOURCE_GENERATOR "TZST")
set(CPACK_VERBATIM_VARIABLES YES)
# get an escaped version of current source directory
string(REGEX REPLACE
  [[\[|\]|\(|\)|\.|\+|\*|\^|\\|\$|\?|\|]] [[\\\0]]
  ESCAPED_CURRENT_SOURCE_DIRECTORY
  ${CMAKE_SOURCE_DIR}
)
set(CPACK_SOURCE_IGNORE_FILES
  ${ESCAPED_CURRENT_SOURCE_DIRECTORY}/build
  ${ESCAPED_CURRENT_SOURCE_DIRECTORY}/install
  ${ESCAPED_CURRENT_SOURCE_DIRECTORY}/\\.git
  ${ESCAPED_CURRENT_SOURCE_DIRECTORY}/\\.vscode
  ${ESCAPED_CURRENT_SOURCE_DIRECTORY}/\\.cache
  ${ESCAPED_CURRENT_SOURCE_DIRECTORY}/\\.clang-format
  ${ESCAPED_CURRENT_SOURCE_DIRECTORY}/\\.gitignore
)
include(CPack)