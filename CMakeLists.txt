cmake_minimum_required(VERSION 3.31)
project(
  gps
  VERSION 0.1.0
  DESCRIPTION "GPU utils"
  HOMEPAGE_URL "https://github.com/changhaoxuan23/gps"
  LANGUAGES CXX
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

option(GPS_ENABLE_GPS     "Build gps, ps for GPUs"                                          ON)
option(GPS_ENABLE_GLAUNCH "Build glaunch, launching helper for programs that utilizes GPUs" ON)
option(GPS_ENABLE_GTRACE  "Build gtrace, GPU API (CUDA API) call tracer"                    ON)

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
  ## first, use nvcc to generate preprocessed, input header
  add_custom_command(
    OUTPUT cuda_apis.hh
    COMMAND ${CUDAToolkit_NVCC_EXECUTABLE} -E ${CMAKE_CURRENT_SOURCE_DIR}/templates/cuda-api-starter.cc -o cuda_apis.hh
    VERBATIM
    CODEGEN
  )
  add_custom_target(generate_hooked_api_input_header DEPENDS cuda_apis.hh)

  ## preprocess the hooked api header
  add_custom_command(
    OUTPUT preprocessed_hooked_api_header.cc
    COMMAND ${CMAKE_CXX_COMPILER} -E ${CMAKE_CURRENT_SOURCE_DIR}/hooked_api_head.cc -I ${CUDAToolkit_INCLUDE_DIRS} -I ${CMAKE_CURRENT_SOURCE_DIR} -DSCANNING -o preprocessed_hooked_api_header.cc
    VERBATIM
    CODEGEN
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/hooked_api_head.cc
  )
  add_custom_target(preprocess_hooked_api_header DEPENDS preprocessed_hooked_api_header.cc)

  ## configure target executable to build hooked APIs
  ##  this is only required if the hooked API source is not provided
  add_executable(hooked_api_builder EXCLUDE_FROM_ALL hooked_api_builder.cc)
  ### find libclang
  find_program(LLVM_CONFIG llvm-config)
  execute_process(
    COMMAND ${LLVM_CONFIG} --cxxflags
    OUTPUT_VARIABLE LLVM_CXXFLAGS
    OUTPUT_STRIP_TRAILING_WHITESPACE
  )
  execute_process(
    COMMAND ${LLVM_CONFIG} --ldflags
    OUTPUT_VARIABLE LLVM_LDFLAGS
    OUTPUT_STRIP_TRAILING_WHITESPACE
  )
  ### link to libclang
  target_compile_options(hooked_api_builder PRIVATE ${LLVM_CXXFLAGS})
  target_link_options(hooked_api_builder PRIVATE ${LLVM_LDFLAGS})
  target_link_libraries(hooked_api_builder PRIVATE clang)

  ## finally build the source file of hooked APIs
  add_custom_command(
    OUTPUT hooked_api.cc
    COMMAND hooked_api_builder cuda_apis.hh ${CMAKE_CURRENT_SOURCE_DIR}/hooked_api_head.cc preprocessed_hooked_api_header.cc hooked_api.cc
    DEPENDS generate_hooked_api_input_header hooked_api_head.cc hooked_api_builder preprocess_hooked_api_header
    VERBATIM
    CODEGEN
  )
  add_custom_target(generate_hooked_api DEPENDS hooked_api.cc)

  ## internal library to handle configurations in hooked API
  add_library(hooked_api_config STATIC hooked_api_config.cc)
  target_include_directories(hooked_api_config PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
  target_compile_definitions(hooked_api_config PRIVATE INSTALL_PREFIX="${CMAKE_INSTALL_PREFIX}")
  # this need to be relocatable to link against hooked_api
  set_target_properties(hooked_api_config PROPERTIES POSITION_INDEPENDENT_CODE ON)

  ## the preloaded shared object of hooked APIs
  add_library(hooked_api SHARED hooked_api.cc)
  target_link_libraries(hooked_api PRIVATE hooked_api_config PUBLIC CUDA::cudart)
  if(Python_FOUND)
    target_compile_definitions(hooked_api PRIVATE HAVE_PYTHON)
    target_link_libraries(hooked_api PRIVATE Python::Python)
  endif()
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