# Compiles the TypeScript site sources into build/share/booru-grabber/sites and copies their
# data files (sites.txt, supported.txt, icon.png, per-site defaults.ini and tag-types.txt).

set(SITES_OUT ${BG_BUILD_DATA}/sites)

# Disabled sources
file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/exclude.txt _excluded REGEX "^[^#].*")

# TypeScript compiler: explicit path, tsc from PATH, or npm (sites/node_modules)
set(_tsc "${BOORU_GRABBER_TSC}")
if(NOT _tsc)
  find_program(_tsc_path tsc)
  if(_tsc_path)
    set(_tsc ${_tsc_path})
  endif()
endif()
set(_npm_stamp "")
if(NOT _tsc)
  find_program(NPM_EXECUTABLE npm REQUIRED)
  set(_tsc ${CMAKE_CURRENT_SOURCE_DIR}/node_modules/.bin/tsc)
  set(_npm_stamp ${CMAKE_CURRENT_BINARY_DIR}/npm.stamp)
  add_custom_command(OUTPUT ${_npm_stamp}
    COMMAND ${NPM_EXECUTABLE} ci --no-audit --no-fund
    COMMAND ${CMAKE_COMMAND} -E touch ${_npm_stamp}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    DEPENDS package.json package-lock.json
    COMMENT "Installing the TypeScript compiler with npm")
endif()
message(STATUS "TypeScript compiler: ${_tsc}")

set(_excludes "\"${CMAKE_CURRENT_SOURCE_DIR}/node_modules\", \"${CMAKE_CURRENT_SOURCE_DIR}/**/*.spec.ts\", \"${CMAKE_CURRENT_SOURCE_DIR}/test-*.ts\"")
foreach(_ex ${_excluded})
  string(STRIP "${_ex}" _ex)
  string(APPEND _excludes ", \"${CMAKE_CURRENT_SOURCE_DIR}/${_ex}\"")
endforeach()
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/tsconfig.build.json "{
  \"extends\": \"${CMAKE_CURRENT_SOURCE_DIR}/tsconfig.json\",
  \"compilerOptions\": { \"outDir\": \"${SITES_OUT}\", \"rootDir\": \"${CMAKE_CURRENT_SOURCE_DIR}\" },
  \"include\": [\"${CMAKE_CURRENT_SOURCE_DIR}/**/*.ts\"],
  \"exclude\": [${_excludes}]
}
")

file(GLOB_RECURSE _ts_sources CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.ts)
list(FILTER _ts_sources EXCLUDE REGEX "/node_modules/")
add_custom_command(OUTPUT ${SITES_OUT}/helper.js
  COMMAND ${_tsc} -p ${CMAKE_CURRENT_BINARY_DIR}/tsconfig.build.json
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  DEPENDS ${_ts_sources} tsconfig.json ${CMAKE_CURRENT_BINARY_DIR}/tsconfig.build.json ${_npm_stamp}
  COMMENT "Compiling site sources")
add_custom_target(sites ALL DEPENDS ${SITES_OUT}/helper.js)

# Data files of the enabled sources
file(GLOB _source_dirs LIST_DIRECTORIES true ${CMAKE_CURRENT_SOURCE_DIR}/*)
foreach(_dir ${_source_dirs})
  if(NOT IS_DIRECTORY ${_dir} OR NOT EXISTS ${_dir}/model.ts)
    continue()
  endif()
  get_filename_component(_name ${_dir} NAME)
  list(FIND _excluded "${_name}" _idx)
  if(NOT _idx EQUAL -1)
    continue()
  endif()
  foreach(_f sites.txt supported.txt icon.png)
    if(EXISTS ${_dir}/${_f})
      configure_file(${_dir}/${_f} ${SITES_OUT}/${_name}/${_f} COPYONLY)
    endif()
  endforeach()
  # Recursive: some site folders are nested (www.newgrounds.com/art)
  file(GLOB_RECURSE _site_files ${_dir}/defaults.ini ${_dir}/tag-types.txt)
  foreach(_f ${_site_files})
    file(RELATIVE_PATH _rel ${_dir} ${_f})
    configure_file(${_f} ${SITES_OUT}/${_name}/${_rel} COPYONLY)
  endforeach()
endforeach()
