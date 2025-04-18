if(GPS_ENABLE_HOOKED_API_BUILDER)
  ## first, use nvcc to generate preprocessed, input header
  add_custom_command(
    OUTPUT cuda_apis.hh
    COMMAND ${CUDAToolkit_NVCC_EXECUTABLE} -E
                                           -o cuda_apis.hh
                                           ${CMAKE_CURRENT_SOURCE_DIR}/cuda-api-starter.cc
    VERBATIM
    CODEGEN
  )
  add_custom_target(generate_hooked_api_input_header DEPENDS cuda_apis.hh)

  ## preprocess the hooked api header
  add_custom_command(
    OUTPUT preprocessed_hooked_api_header.cc
    COMMAND ${CMAKE_CXX_COMPILER} -E ${CMAKE_CURRENT_SOURCE_DIR}/hooked_api_head.cc
                                  -I ${CUDAToolkit_INCLUDE_DIRS}
                                  -I ${CMAKE_CURRENT_SOURCE_DIR}
                                  -DSCANNING
                                  -o preprocessed_hooked_api_header.cc
    VERBATIM
    CODEGEN
    DEPENDS hooked_api_head.cc
  )
  add_custom_target(preprocess_hooked_api_header DEPENDS preprocessed_hooked_api_header.cc)

  ## configure target executable to build hooked APIs
  add_executable(hooked_api_builder EXCLUDE_FROM_ALL hooked_api_builder.cc)
  ### find and link to libclang
  find_package(Clang REQUIRED)
  target_link_libraries(hooked_api_builder PRIVATE libclang)

  ## finally build the source file of hooked APIs
  add_custom_command(
    OUTPUT ${CMAKE_SOURCE_DIR}/hooked_api.cc
    COMMAND hooked_api_builder cuda_apis.hh
                               ${CMAKE_CURRENT_SOURCE_DIR}/hooked_api_head.cc
                               preprocessed_hooked_api_header.cc
                               ${CMAKE_SOURCE_DIR}/hooked_api.cc
    DEPENDS generate_hooked_api_input_header
            hooked_api_head.cc
            hooked_api_builder
            preprocess_hooked_api_header
    VERBATIM
    CODEGEN
  )
  add_custom_target(build_hooked_api_source DEPENDS ${CMAKE_SOURCE_DIR}/hooked_api.cc)
endif()

## internal library to handle configurations in hooked API
add_library(hooked_api_config STATIC ${CMAKE_SOURCE_DIR}/hooked_api_config.cc)
target_include_directories(hooked_api_config PUBLIC ${CMAKE_SOURCE_DIR})
target_compile_definitions(hooked_api_config PRIVATE INSTALL_PREFIX="${CMAKE_INSTALL_PREFIX}")
# this need to be relocatable to link against hooked_api
set_target_properties(hooked_api_config PROPERTIES POSITION_INDEPENDENT_CODE ON)

## the preloaded shared object of hooked APIs
add_library(hooked_api SHARED ${CMAKE_SOURCE_DIR}/hooked_api.cc)
target_link_libraries(hooked_api PRIVATE hooked_api_config PUBLIC CUDA::cudart)
if(Python_FOUND)
  target_compile_definitions(hooked_api PRIVATE HAVE_PYTHON)
  target_link_libraries(hooked_api PRIVATE Python::Python)
endif()