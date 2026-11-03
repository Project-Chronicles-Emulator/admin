# Module targets (design sections 3.4 and 4). Every folder under src/ listed in
# tools/source_layout.json is one library named chronicles-<folder with / replaced by ->.
# DEPENDS lists the module folders whose headers the module includes.
# Keep one argument per line so dependency changes remain easy to review.

include_guard(GLOBAL)
include(GoogleTest)

function(chronicles_module_target folder out_variable)
  string(REPLACE "/" "-" target_suffix "${folder}")
  set(${out_variable} "chronicles-${target_suffix}" PARENT_SCOPE)
endfunction()

function(chronicles_module_targets out_variable)
  set(targets)
  foreach(folder IN LISTS ARGN)
    chronicles_module_target("${folder}" target)
    list(APPEND targets "${target}")
  endforeach()
  set(${out_variable} ${targets} PARENT_SCOPE)
endfunction()

# chronicles_add_module(<folder>
#   [SOURCES <file>...]            sources relative to the calling CMakeLists.txt
#   [DEPENDS <module folder>...]   modules whose headers this module includes
#   [LIBRARIES <target>...]        third-party or system libraries it uses
#   [DEFINITIONS <definition>...]  definitions its headers need (propagated to dependents)
#   [PRECOMPILED_HEADERS])         compile cmake/chronicles_pch.hpp (needs json and spdlog)
# A module without sources becomes an INTERFACE library.
function(chronicles_add_module folder)
  cmake_parse_arguments(PARSE_ARGV 1 module "PRECOMPILED_HEADERS" ""
                        "SOURCES;DEPENDS;LIBRARIES;DEFINITIONS")
  if(module_UNPARSED_ARGUMENTS)
    message(FATAL_ERROR
      "chronicles_add_module(${folder}): unknown arguments ${module_UNPARSED_ARGUMENTS}")
  endif()

  chronicles_module_target("${folder}" target)
  chronicles_module_targets(dependency_targets ${module_DEPENDS})

  if(module_SOURCES)
    add_library(${target} STATIC ${module_SOURCES})
    set(scope PUBLIC)
  else()
    add_library(${target} INTERFACE)
    set(scope INTERFACE)
  endif()

  # Includes are written relative to src/, so every module exposes the same root.
  target_include_directories(${target} ${scope} "${PROJECT_SOURCE_DIR}/src")
  target_link_libraries(${target} ${scope} ${dependency_targets} ${module_LIBRARIES})
  if(module_DEFINITIONS)
    target_compile_definitions(${target} ${scope} ${module_DEFINITIONS})
  endif()
  if(module_PRECOMPILED_HEADERS AND module_SOURCES)
    chronicles_use_precompiled_headers(${target})
  endif()
endfunction()

# chronicles_add_module_tests(<folder>
#   SOURCES <file>...              test sources relative to the calling CMakeLists.txt
#   [MODULES <module folder>...]   other modules the tests include (for example testing)
#   [LIBRARIES <target>...]        extra libraries
#   [NAME <target>]                defaults to chronicles-<folder>-tests
#   [TEST_PREFIX <prefix>])        CTest name prefix, kept for historical test names
# Every test program gets the same definitions; no test changes behavior on their presence.
function(chronicles_add_module_tests folder)
  # GTest is only found when tests are configured; module CMakeLists call this
  # unconditionally, so without tests there is nothing to build.
  if(NOT BUILD_TESTING)
    return()
  endif()
  cmake_parse_arguments(PARSE_ARGV 1 tests "" "NAME;TEST_PREFIX" "SOURCES;MODULES;LIBRARIES")
  if(tests_UNPARSED_ARGUMENTS OR NOT tests_SOURCES)
    message(FATAL_ERROR "chronicles_add_module_tests(${folder}): SOURCES required, "
      "unknown arguments: ${tests_UNPARSED_ARGUMENTS}")
  endif()

  chronicles_module_target("${folder}" module_target)
  chronicles_module_targets(extra_targets ${tests_MODULES})
  if(tests_NAME)
    set(target "${tests_NAME}")
  else()
    set(target "${module_target}-tests")
  endif()

  add_executable(${target} ${tests_SOURCES})
  target_link_libraries(${target} PRIVATE
    ${module_target}
    ${extra_targets}
    ${tests_LIBRARIES}
    nlohmann_json::nlohmann_json
    spdlog::spdlog
    GTest::gtest_main
  )
  target_compile_definitions(${target} PRIVATE
    CHRONICLES_PROJECT_ROOT="${PROJECT_SOURCE_DIR}"
    CHRONICLES_PROTOCOL_DIR="${PROJECT_SOURCE_DIR}/protocol"
    CHRONICLES_MIGRATION_FILE="${PROJECT_SOURCE_DIR}/migrations/001_accounts_characters.sql"
  )
  chronicles_use_test_precompiled_headers(${target})

  if(tests_TEST_PREFIX)
    gtest_discover_tests(${target} TEST_PREFIX "${tests_TEST_PREFIX}")
  else()
    gtest_discover_tests(${target})
  endif()
endfunction()
