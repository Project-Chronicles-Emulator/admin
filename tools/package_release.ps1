#requires -Version 7.2
param([string]$BuildDirectory = 'out/build/windows')

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $root
$component = 'admin'
$package = Join-Path $root "out/release/$component-win64"
if (Test-Path -LiteralPath $package) {
    throw "Package directory already exists: $package. Use a fresh output directory."
}
New-Item -ItemType Directory -Path $package -Force | Out-Null

function Copy-Required {
    param([string]$Source, [string]$Destination = $package)

    if (-not (Test-Path -LiteralPath $Source)) {
        throw "Required release input is missing: $Source"
    }
    Copy-Item -LiteralPath $Source -Destination $Destination -Recurse -Force
}

$binary = Join-Path $BuildDirectory 'bin/Release/admin'
Copy-Required "$binary/chronicles-admin.exe"
Get-ChildItem -LiteralPath $binary -Filter '*.dll' -File | Copy-Item -Destination $package
Copy-Required 'admin/web'

Copy-Required 'README.md'
if (Test-Path -LiteralPath 'LICENSE') {
    Copy-Required 'LICENSE'
}
# Package only explicitly selected binaries, configuration examples and tool source.
# Never copy the checkout, data/generated, private configuration or research exports.
$archive = "$package.zip"
Compress-Archive -Path "$package/*" -DestinationPath $archive
$hash = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
"$hash  $([IO.Path]::GetFileName($archive))" | Set-Content -LiteralPath "$archive.sha256" -Encoding ascii
Write-Output $archive
