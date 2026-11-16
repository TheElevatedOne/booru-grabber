cmake_minimum_required(VERSION 3.16)
project(booru-grabber VERSION 1.0.0
    DESCRIPTION "Imageboard/booru downloader with powerful file naming, GTK 3 rewrite of Grabber"
    HOMEPAGE_URL "https://github.com/TheElevatedOne/booru-grabber"
    LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(BOORU_GRABBER_GUI "Build the GTK 3 interface" ON)
option(BOORU_GRABBER_TESTS "Build the test suite" ON)
option(BOORU_GRABBER_TRAY "Tray icon through libayatana-appindicator (if found)" ON)
option(BOORU_GRABBER_RELOCATABLE "Use $ORIGIN-relative RPATHs so the installed tree can be moved" ON)
option(BOORU_GRABBER_PORTABLE "Install as a self-contained directory that keeps its profile next to the binaries" OFF)
option(BOORU_GRABBER_FETCH_QUICKJS "Download and build quickjs-ng if it is not installed" ON)
set(BOORU_GRABBER_TSC "" CACHE FILEPATH "TypeScript compiler used to build the site sources (default: tsc from PATH, then npm)")

include(GNUInstallDirs)
find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)

# ---- install layout ----------------------------------------------------------
# All layouts keep bin/, lib/ and share/ relative to each other, so the binaries find their data
# through $ORIGIN/../share in a /usr install, an arbitrary --prefix directory and an AppImage alike.
# The portable layout flattens the binaries into the directory root and adds a "portable" marker.
if(BOORU_GRABBER_PORTABLE)
  set(BG_BINDIR ".")
  set(BG_LIBDIR "lib")
  set(BG_DATADIR "share")
  set(BG_SYSCONFDIR "etc")
  set(BG_RPATH "$ORIGIN/lib")
else()
  set(BG_BINDIR "${CMAKE_INSTALL_BINDIR}")
  set(BG_LIBDIR "${CMAKE_INSTALL_LIBDIR}")
  set(BG_DATADIR "${CMAKE_INSTALL_DATADIR}")
  set(BG_SYSCONFDIR "${CMAKE_INSTALL_SYSCONFDIR}")
  file(RELATIVE_PATH _bin_to_lib "${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}" "${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}")
  set(BG_RPATH "$ORIGIN/${_bin_to_lib}")
endif()
if(BOORU_GRABBER_RELOCATABLE OR BOORU_GRABBER_PORTABLE)
  set(CMAKE_INSTALL_RPATH "${BG_RPATH}")
  set(CMAKE_BUILD_WITH_INSTALL_RPATH OFF)
endif()

# Build tree mirrors the install tree: build/bin, build/lib, build/share/booru-grabber
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(BG_BUILD_DATA ${CMAKE_BINARY_DIR}/share/booru-grabber)

if(IS_ABSOLUTE "${CMAKE_INSTALL_FULL_SYSCONFDIR}")
  set(BG_FULL_SYSCONFDIR "${CMAKE_INSTALL_FULL_SYSCONFDIR}")
else()
  set(BG_FULL_SYSCONFDIR "${CMAKE_INSTALL_PREFIX}/${BG_SYSCONFDIR}")
endif()
set(BG_FULL_DATADIR "${CMAKE_INSTALL_PREFIX}/${BG_DATADIR}")

# ---- dependencies ------------------------------------------------------------
pkg_check_modules(GLIB REQUIRED IMPORTED_TARGET glib-2.0>=2.74 gio-2.0 gio-unix-2.0)
pkg_check_modules(SOUP REQUIRED IMPORTED_TARGET libsoup-3.0>=3.0)
pkg_check_modules(LIBXML REQUIRED IMPORTED_TARGET libxml-2.0)
pkg_check_modules(SQLITE REQUIRED IMPORTED_TARGET sqlite3)
pkg_check_modules(PIXBUF REQUIRED IMPORTED_TARGET gdk-pixbuf-2.0)
find_package(nlohmann_json 3.9 REQUIRED)

# quickjs-ng: system library (Arch: quickjs-ng) or a pinned download
find_path(QUICKJS_INCLUDE_DIR quickjs.h)
find_library(QUICKJS_LIBRARY NAMES qjs)
if(QUICKJS_INCLUDE_DIR AND QUICKJS_LIBRARY)
  message(STATUS "Using system quickjs-ng: ${QUICKJS_LIBRARY}")
  add_library(bg_quickjs INTERFACE)
  target_include_directories(bg_quickjs INTERFACE ${QUICKJS_INCLUDE_DIR})
  target_link_libraries(bg_quickjs INTERFACE ${QUICKJS_LIBRARY})
elseif(BOORU_GRABBER_FETCH_QUICKJS)
  message(STATUS "quickjs-ng not found, building v0.18.0 from source")
  include(FetchContent)
  set(QJS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(QJS_BUILD_LIBC OFF CACHE BOOL "" FORCE)
  set(BUILD_SHARED_LIBS_SAVED ${BUILD_SHARED_LIBS})
  set(BUILD_SHARED_LIBS OFF)
  FetchContent_Declare(quickjs GIT_REPOSITORY https://github.com/quickjs-ng/quickjs.git GIT_TAG v0.18.0 GIT_SHALLOW TRUE)
  FetchContent_GetProperties(quickjs)
  if(NOT quickjs_POPULATED)
    FetchContent_Populate(quickjs)
    add_subdirectory(${quickjs_SOURCE_DIR} ${quickjs_BINARY_DIR} EXCLUDE_FROM_ALL)
  endif()
  set(BUILD_SHARED_LIBS ${BUILD_SHARED_LIBS_SAVED})
  add_library(bg_quickjs INTERFACE)
  target_link_libraries(bg_quickjs INTERFACE qjs)
else()
  message(FATAL_ERROR "quickjs-ng not found (install quickjs-ng or enable BOORU_GRABBER_FETCH_QUICKJS)")
endif()

# lexbor HTML parser, vendored as a git submodule and linked statically
if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/vendor/lexbor/CMakeLists.txt)
  message(FATAL_ERROR "vendor/lexbor is missing, run: git submodule update --init")
endif()
set(LEXBOR_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(LEXBOR_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(LEXBOR_INSTALL_HEADERS OFF CACHE BOOL "" FORCE)
set(LEXBOR_BUILD_TESTS_CPP OFF CACHE BOOL "" FORCE)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)  # lexbor still declares cmake_minimum_required(2.8)
add_subdirectory(vendor/lexbor EXCLUDE_FROM_ALL)
unset(CMAKE_POLICY_VERSION_MINIMUM)

configure_file(src/config.hpp.in ${CMAKE_CURRENT_BINARY_DIR}/config.hpp)

# ---- core library ------------------------------------------------------------
file(GLOB BG_CORE_SOURCES CONFIGURE_DEPENDS src/core/*.cpp)
add_library(booru-grabber-core SHARED ${BG_CORE_SOURCES} vendor/html-entities/html-entities.cpp)
set_target_properties(booru-grabber-core PROPERTIES
  OUTPUT_NAME booru-grabber
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR})
target_include_directories(booru-grabber-core
  PUBLIC src ${CMAKE_CURRENT_BINARY_DIR}
  PRIVATE vendor/html-entities vendor/lexbor/source)
target_compile_options(booru-grabber-core PRIVATE -Wall -Wextra)
target_link_libraries(booru-grabber-core
  PUBLIC PkgConfig::GLIB PkgConfig::PIXBUF nlohmann_json::nlohmann_json
  PRIVATE PkgConfig::SOUP PkgConfig::LIBXML PkgConfig::SQLITE bg_quickjs lexbor_static Threads::Threads)
install(TARGETS booru-grabber-core LIBRARY DESTINATION ${BG_LIBDIR} NAMELINK_SKIP)

# ---- site sources (TypeScript -> JavaScript) ---------------------------------
add_subdirectory(sites)

# ---- runtime data ------------------------------------------------------------
file(COPY data/words.txt DESTINATION ${BG_BUILD_DATA})
file(COPY data/webservices DESTINATION ${BG_BUILD_DATA})
file(COPY data/images DESTINATION ${BG_BUILD_DATA})
file(COPY data/icons/hicolor/128x128/apps/booru-grabber.png DESTINATION ${BG_BUILD_DATA})
install(DIRECTORY ${BG_BUILD_DATA}/ DESTINATION ${BG_DATADIR}/booru-grabber)
if(BOORU_GRABBER_PORTABLE)
  install(FILES data/settings.ini DESTINATION ${BG_SYSCONFDIR}/booru-grabber)
else()
  # /etc for a /usr prefix (GNUInstallDirs special case), <prefix>/etc otherwise
  install(FILES data/settings.ini DESTINATION ${CMAKE_INSTALL_FULL_SYSCONFDIR}/booru-grabber)
endif()

# ---- CLI ---------------------------------------------------------------------
add_executable(booru-grabber-cli src/cli/main.cpp)
target_compile_options(booru-grabber-cli PRIVATE -Wall -Wextra)
target_link_libraries(booru-grabber-cli PRIVATE booru-grabber-core)
add_dependencies(booru-grabber-cli sites)
install(TARGETS booru-grabber-cli RUNTIME DESTINATION ${BG_BINDIR})

# ---- GUI ---------------------------------------------------------------------
if(BOORU_GRABBER_GUI)
  add_subdirectory(src/gui)
endif()

if(NOT BOORU_GRABBER_PORTABLE)
  install(FILES data/io.github.TheElevatedOne.BooruGrabber.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
  install(FILES data/io.github.TheElevatedOne.BooruGrabber.metainfo.xml DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)
  install(DIRECTORY data/icons/hicolor DESTINATION ${CMAKE_INSTALL_DATADIR}/icons)
else()
  install(FILES data/portable DESTINATION ${BG_BINDIR})
endif()

# ---- tests -------------------------------------------------------------------
if(BOORU_GRABBER_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
