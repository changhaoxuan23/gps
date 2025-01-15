add_library(
  gps-internals STATIC
  process_information.cc
  filter.cc
)

target_include_directories(gps-internals PUBLIC ${CMAKE_CURRENT_LIST_DIR})
target_link_libraries(gps-internals PRIVATE gps-utils PRIVATE nvml_common)

target_link_libraries(gps PRIVATE gps-internals)