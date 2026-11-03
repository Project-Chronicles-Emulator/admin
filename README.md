# Project Chronicles Admin

Standalone Windows desktop application for Project Chronicles.

Requires Visual Studio 2026 with C++ tools, CMake 3.25+, Ninja, Python, and vcpkg.
The pinned manifest installs the C++ dependencies, including WebView2.

```powershell
. .\tools\dev_shell.ps1
cmake --preset windows -DCMAKE_TOOLCHAIN_FILE=C:/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build --preset release --target chronicles-admin
```

The executable and its web interface are under `out/build/windows/bin/Release/admin/`.
Configure the server address locally. Credentials, generated output and client binaries
are excluded from this repository.

Shared desktop/protocol helpers needed by this application are included in this checkout;
no sibling repository is required to build it. Backend data and database installation
belong to the separate `game-server` repository.

Default local service: http://127.0.0.1:8082 (game-server).
Existing saved server settings are preserved.
