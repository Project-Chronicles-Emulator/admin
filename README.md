<p align="center"><img src="https://raw.githubusercontent.com/Project-Chronicles-Emulator/game-server/main/docs/assets/project-chronicles-hero.png" alt="Project Chronicles fantasy harbor" width="100%"></p>

<h1 align="center">Project Chronicles · Admin</h1>

<p align="center"><a href="https://github.com/Project-Chronicles-Emulator/game-server/blob/main/README.md">Project home</a> · <a href="https://github.com/Project-Chronicles-Emulator/game-server/blob/main/docs/guides/getting-started.md">Complete setup</a> · <a href="https://github.com/Project-Chronicles-Emulator/game-server/blob/main/docs/progress.md">Progress</a> · <a href="https://github.com/Project-Chronicles-Emulator/game-server/blob/main/CONTRIBUTING.md">Contribute</a></p>

Windows WebView2 operator console for the game server. Uses a private operator token, separate from player credentials and the login authority key.

## Build this component

Requires Windows x64, Visual Studio 2026 C++ tools, CMake 3.25+, Ninja, Python and
vcpkg and WebView2 Runtime.
Each checkout includes its source dependencies; no sibling checkout is needed to compile.

```powershell
. .\tools\dev_shell.ps1
cmake --preset windows -DCMAKE_TOOLCHAIN_FILE=C:/dev/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build --preset release --target chronicles-admin
```

Replace the vcpkg path with your local installation. Keep the built executable,
DLLs and web assets together.

## Run

First complete the [shared environment setup](https://github.com/Project-Chronicles-Emulator/game-server/blob/main/docs/guides/getting-started.md).
It covers database installation, service keys, world TLS and startup order.

```powershell
.\out\build\windows\bin\Release\admin\chronicles-admin.exe --server http://127.0.0.1:8082 --token-file C:/ProjectChronicles/private/admin-token.key
```

Connect to **game HTTP at http://127.0.0.1:8082**. Game must enable the operator API
with the same `--admin-token-file`. Existing saved connections are preserved; choose
the new endpoint explicitly if necessary. [Admin guide](https://github.com/Project-Chronicles-Emulator/game-server/blob/main/docs/guides/components.md#admin).

## Work on the project

Read the [architecture](https://github.com/Project-Chronicles-Emulator/game-server/blob/main/docs/guides/architecture.md),
[feature/bug workflow](https://github.com/Project-Chronicles-Emulator/game-server/blob/main/docs/guides/feature-workflow.md) and
[testing guide](https://github.com/Project-Chronicles-Emulator/game-server/blob/main/docs/guides/testing.md). Report the exact component, commit,
reproduction and redacted evidence. Build/synthetic checks and original-client
observations are distinct; the service split has not had a fresh original-client
gameplay acceptance trial. Keep local credentials and generated/client files out of commits.
