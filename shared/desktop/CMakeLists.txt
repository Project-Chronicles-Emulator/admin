add_library(chronicles-launcher-core STATIC
    encoding.cpp
    game_process.cpp
    http_client.cpp
    patch_installer.cpp
    playsharp_bridge.cpp
    prerequisites.cpp
    webview_host.cpp
    windows_command_line.cpp
)
target_include_directories(chronicles-launcher-core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/third_party/xorstr")
target_compile_definitions(chronicles-launcher-core PUBLIC
    _WIN32_WINNT=0x0A00 NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE
    JM_XORSTR_DISABLE_AVX_INTRINSICS)
target_link_libraries(chronicles-launcher-core PUBLIC
    chronicles-distribution nlohmann_json::nlohmann_json unofficial::webview2::webview2
    advapi32 bcrypt crypt32 comctl32 dwmapi ole32 shell32 shlwapi user32 winhttp
    iphlpapi ws2_32)
