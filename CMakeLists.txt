cmake_minimum_required(VERSION 3.29)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_CXX_COMPILER clang++)

project(gps VERSION 1.0.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# set compile options based on build type
set(CMAKE_CXX_FLAGS_DEBUG -g3)
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG -flto=full")
if(NOT DEFINED CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

add_compile_options(-fPIC -stdlib=libc++)
add_link_options(-fuse-ld=lld -stdlib=libc++)

find_package(ConfigurationsPP REQUIRED)
find_package(Threads REQUIRED)
find_package(CUDAToolkit REQUIRED)

find_package(SecretStorageAccessor)

add_library(nvml_common STATIC nvml_common.cc)
target_include_directories(nvml_common PUBLIC "${CUDAToolkit_INCLUDE_DIRS}")
target_link_libraries(nvml_common PRIVATE CUDA::nvml)

add_executable(gps gps.cc)
target_link_libraries(gps PRIVATE nvml_common)

add_executable(glaunch glaunch.cc)
target_link_libraries(glaunch PRIVATE ConfigurationsPP nvml_common Threads::Threads)
if(DEFINED SecretStorageAccessor_FOUND)
  target_compile_definitions(glaunch PRIVATE HAVE_SECRET_STORAGE)
  target_link_libraries(glaunch PRIVATE SecretStorageAccessor)
endif()

install(TARGETS gps glaunch
  RUNTIME DESTINATION bin
)



add_executable(test test.cc)
target_link_libraries(test PRIVATE nvml_common)