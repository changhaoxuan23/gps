add_library(
  glaunch-submodules STATIC
  glaunch-module.cc
  logging.cc
  memory-watcher.cc
  timing.cc
  email-notifier.cc
)

target_include_directories(glaunch-submodules PUBLIC ${CMAKE_CURRENT_LIST_DIR})
target_link_libraries(
  glaunch-submodules
  PRIVATE nvml_common
  PRIVATE hooked_api_config
  PRIVATE ConfigurationsPP
)
if(keyutils_FOUND AND Python_FOUND)
  target_compile_definitions(glaunch-submodules PRIVATE EMAIL_NOTIFIER_SHOULD_WORK)
  target_link_libraries(glaunch-submodules PRIVATE PkgConfig::keyutils Python::Python)
  set(GPS_EMAIL_NOTIFIER_ENABLED YES PARENT_SCOPE)
endif()

target_link_libraries(glaunch PRIVATE glaunch-submodules)