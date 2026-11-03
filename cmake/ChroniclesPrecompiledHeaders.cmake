# Precompiled headers for emulator targets (design section 4). Each target compiles its own
# copy: targets differ in definitions and options, which MSVC requires to match for a shared
# precompiled header.

set(CHRONICLES_PCH_HEADER "${CMAKE_CURRENT_LIST_DIR}/chronicles_pch.hpp")
set(CHRONICLES_TEST_PCH_HEADER "${CMAKE_CURRENT_LIST_DIR}/chronicles_test_pch.hpp")

# Targets must already link nlohmann_json and spdlog (directly or through the module
# libraries their sources include).
function(chronicles_use_precompiled_headers)
  foreach(target IN LISTS ARGN)
    target_precompile_headers(${target} PRIVATE "${CHRONICLES_PCH_HEADER}")
  endforeach()
endfunction()

# Test programs must also link GoogleTest.
function(chronicles_use_test_precompiled_headers)
  foreach(target IN LISTS ARGN)
    target_precompile_headers(${target} PRIVATE "${CHRONICLES_TEST_PCH_HEADER}")
  endforeach()
endfunction()
