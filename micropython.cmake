add_library(usermod_extended_blockdev INTERFACE)

target_sources(usermod_extended_blockdev INTERFACE
  ${CMAKE_CURRENT_LIST_DIR}/ebdev.c
)

target_include_directories(usermod_extended_blockdev INTERFACE
  ${CMAKE_CURRENT_LIST_DIR}/
)

target_link_libraries(usermod INTERFACE usermod_extended_blockdev)
