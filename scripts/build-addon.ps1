<#
Build from an x64 Visual Studio Developer PowerShell with clang-cl, CMake,
Ninja, Windows SDK and RenoDX's shader tools installed. Requires the full
pinned RenoDX checkout including submodules; see the Windows build workflow.
-CheckOnly validates the shared-source API without compiling.
#>
[CmdletBinding()]
param(
  [Parameter(Mandatory = $true)]
  [string]$RenoDXRoot,
  [ValidateSet('Debug', 'Release')]
  [string]$Configuration = 'Release',
  [switch]$CheckOnly
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = (Resolve-Path -LiteralPath $RenoDXRoot).Path
$source = Split-Path -Parent $PSScriptRoot

# Addon-local adapters supply the snapshot's COM identity and Detours helpers.
$requirements = @{
  'src/utils/directx.hpp' = @('NativeFromReShadeProxy', 'ReShadeRetrieveBaseInterface')
}
$missing = @()
foreach ($relative in $requirements.Keys) {
  $path = Join-Path $root $relative
  if (!(Test-Path -LiteralPath $path -PathType Leaf)) {
    $missing += $relative
    continue
  }
  $content = Get-Content -Raw -LiteralPath $path
  foreach ($symbol in $requirements[$relative]) {
    if ($content -notmatch ('\b' + [regex]::Escape($symbol) + '\b')) {
      $missing += "$relative : $symbol"
    }
  }
}
if ($missing.Count -ne 0) {
  throw ("Incompatible RenoDX checkout. Missing APIs required by v8.5dev:`n - " +
    ($missing -join "`n - ") + "`nUse the RenoDX revision pinned in .github/workflows/build-windows.yml.")
}
if ($CheckOnly) {
  Write-Host 'Shared-source API preflight passed. This is not a compiler or runtime test.'
  return
}
if ($env:OS -ne 'Windows_NT') {
  throw 'The addon target requires the Windows x64 toolchain.'
}
foreach ($command in @('cmake', 'ninja', 'clang-cl', 'nmake')) {
  if (!(Get-Command $command -ErrorAction SilentlyContinue)) {
    throw "Missing $command. Use an x64 Visual Studio Developer PowerShell with LLVM and CMake/Ninja."
  }
}
foreach ($relative in @(
    'CMakePresets.json', 'external/reshade/include/reshade.hpp',
    'external/DLSS/include/nvsdk_ngx.h', 'external/Streamline/include/sl.h',
    'external/Detours/src/detours.cpp')) {
  if (!(Test-Path -LiteralPath (Join-Path $root $relative) -PathType Leaf)) {
    throw "Missing $relative. Initialize the compatible checkout's submodules first."
  }
}

$target = Join-Path $root 'src/addons/dlss5'
if ([IO.Path]::GetFullPath($source) -ne [IO.Path]::GetFullPath($target)) {
  $marker = Join-Path $target '.fallback-source'
  if (Test-Path -LiteralPath $target) {
    if (!(Test-Path -LiteralPath $marker) -or (Get-Content -Raw -LiteralPath $marker).Trim() -ne $source) {
      throw "Refusing to overwrite existing addon sources at $target. Use a separate build checkout."
    }
  } else {
    New-Item -ItemType Directory -Path $target -Force | Out-Null
    Set-Content -LiteralPath $marker -Value $source
  }
  Get-ChildItem -LiteralPath $source -File | Where-Object {
    $_.Extension -in @('.cpp', '.hpp', '.h') -or $_.Name -eq 'LICENSE'
  } | Copy-Item -Destination $target -Force
  foreach ($directory in @('shaders', 'ui', 'tests')) {
    Copy-Item -LiteralPath (Join-Path $source $directory) -Destination $target -Recurse -Force
  }
}

Push-Location $root
try {
  & cmake --preset clang-x64 "-DCMAKE_MODULE_LINKER_FLAGS=/machine:x64 /DELAYLOAD:winhttp.dll"
  if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed; check the first error above.' }
  $preset = 'clang-x64-' + $Configuration.ToLowerInvariant()
  & cmake --build --preset $preset --target detours_build
  if ($LASTEXITCODE -ne 0) { throw 'Detours build failed.' }
  $testOutput = Join-Path $root 'build/fallback-tests'
  New-Item -ItemType Directory -Path $testOutput -Force | Out-Null
  Push-Location $testOutput
  try {
    & clang-cl /nologo /std:c++20 /EHsc /W4 /MT /DNOMINMAX `
      (Join-Path $target 'tests/present_fallback_test.cpp') /Fe:present_fallback_test.exe
    if ($LASTEXITCODE -ne 0) { throw 'Routing test compilation failed.' }
    & .\present_fallback_test.exe
    if ($LASTEXITCODE -ne 0) { throw 'Routing tests failed.' }
    & clang-cl /nologo /std:c++20 /EHsc /W4 /MT /DNOMINMAX `
      ("/I" + (Join-Path $root 'external/Detours/include')) `
      (Join-Path $target 'tests/windows_compat_test.cpp') `
      (Join-Path $root 'external/Detours/lib.X64/detours.lib') /Fe:windows_compat_test.exe
    if ($LASTEXITCODE -ne 0) { throw 'Windows compatibility test compilation failed.' }
    & .\windows_compat_test.exe
    if ($LASTEXITCODE -ne 0) { throw 'Windows compatibility tests failed.' }
  } finally {
    Pop-Location
  }
  & cmake --build --preset $preset --target dlss5
  if ($LASTEXITCODE -ne 0) { throw 'Addon compilation failed; check the first error above.' }
  $binary = Join-Path $root "build/$Configuration/renodx-dlss5.addon64"
  if (!(Test-Path -LiteralPath $binary)) { throw "Build returned success but $binary is missing." }
  Write-Host "Built: $binary"
  Write-Host 'This builds the addon only; it does not install the NVIDIA runtime or modify the game.'
} finally {
  Pop-Location
}
