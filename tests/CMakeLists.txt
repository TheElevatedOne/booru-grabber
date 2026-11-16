find_package(Catch2 3 QUIET)
if(NOT Catch2_FOUND)
  message(STATUS "Catch2 3 not found, unit tests disabled")
  return()
endif()

add_executable(booru-grabber-tests
  settings_test.cpp
  datetime_test.cpp
  filename_test.cpp
  filters_test.cpp
  sources_test.cpp)
target_compile_options(booru-grabber-tests PRIVATE -Wall -Wextra)
target_compile_definitions(booru-grabber-tests PRIVATE
  BG_TEST_SITES_SOURCE="${CMAKE_SOURCE_DIR}/sites"
  BG_TEST_SITES_BUILD="${BG_BUILD_DATA}/sites")
target_link_libraries(booru-grabber-tests PRIVATE booru-grabber-core Catch2::Catch2WithMain)
add_dependencies(booru-grabber-tests sites)
add_test(NAME unit COMMAND booru-grabber-tests)
