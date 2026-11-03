# Project Chronicles Server Admin: WebView2 desktop console for the server's /admin/v1 API.
# It reuses the launcher's WebView2 host, WinHTTP client and encoding helpers.

# The admin console loads web/ from beside its executable, like the launcher.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>/admin")

add_executable(chronicles-admin WIN32
  src/main.cpp
  src/console_client.cpp
  src/ssh_transport.cpp
)
target_link_libraries(chronicles-admin PRIVATE chronicles-launcher-core)
chronicles_process_resources(chronicles-admin admin "Project Chronicles Server Admin")
target_compile_definitions(chronicles-admin PRIVATE
  CHRONICLES_ADMIN_PROJECT_ROOT=L"${PROJECT_SOURCE_DIR}"
)

# The executable loads its interface from the adjacent web folder, like the launcher.
add_custom_target(chronicles-admin-assets
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${CMAKE_CURRENT_SOURCE_DIR}/web"
    "$<TARGET_FILE_DIR:chronicles-admin>/web"
  COMMENT "Copying Project Chronicles Server Admin interface"
)
add_dependencies(chronicles-admin chronicles-admin-assets)

install(TARGETS chronicles-admin RUNTIME DESTINATION admin COMPONENT Admin)
install(DIRECTORY web DESTINATION admin COMPONENT Admin)
install(FILES "$<TARGET_FILE_DIR:chronicles-admin>/WebView2Loader.dll"
  DESTINATION admin COMPONENT Admin
)
