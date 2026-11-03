<#
.SYNOPSIS
Loads the Visual Studio x64 developer environment into the current PowerShell session.

.DESCRIPTION
The Ninja generator runs cl.exe, link.exe and rc.exe directly, so they and the Windows SDK
must be on PATH. Dot-source this script once per terminal before configuring or building:

    . .\tools\dev_shell.ps1

It does nothing when the x64 environment is already loaded.
#>

if ($env:VSCMD_ARG_TGT_ARCH -eq 'x64' -and (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    return
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
    throw "vswhere.exe was not found at $vswhere. Install Visual Studio 2026 with C++ tools."
}

$installationPath = & $vswhere -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if ([string]::IsNullOrWhiteSpace($installationPath)) {
    throw 'No Visual Studio installation with the x64 C++ tools was found.'
}

$devShellModule = Join-Path $installationPath 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll'
Import-Module $devShellModule -ErrorAction Stop
Enter-VsDevShell -VsInstallPath $installationPath -SkipAutomaticLocation `
    -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null

if (-not (Get-Command ninja.exe -ErrorAction SilentlyContinue)) {
    throw 'ninja.exe is not on PATH after loading the developer environment.'
}
