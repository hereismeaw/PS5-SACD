param(
    [string]$SdkPath = [Environment]::GetEnvironmentVariable('PS5_PAYLOAD_SDK', 'User'),
    [string]$LlvmBin = 'C:\Program Files\LLVM\bin',
    [string]$GitBash = 'C:\Program Files\Git\bin\bash.exe',
    [string]$MusicUiUrl
)

$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrWhiteSpace($SdkPath)) {
    throw 'Pass -SdkPath or set the PS5_PAYLOAD_SDK user environment variable.'
}
$SdkPath = [IO.Path]::GetFullPath($SdkPath)
if (-not (Test-Path -LiteralPath (Join-Path $SdkPath 'toolchain\prospero.mk'))) {
    throw "No ps5-payload-sdk installation found at $SdkPath"
}
if (-not (Test-Path -LiteralPath $GitBash)) {
    throw "Git Bash was not found at $GitBash"
}
$clang = Join-Path $LlvmBin 'clang++.exe'
if (-not (Test-Path -LiteralPath $clang)) {
    throw "LLVM clang++ was not found at $clang"
}
$musicUri = $null
if (-not [Uri]::TryCreate($MusicUiUrl, [UriKind]::Absolute, [ref]$musicUri)) {
    throw 'Pass -MusicUiUrl as an absolute HTTP or HTTPS URL reachable by the PS5.'
}
if ($musicUri.Scheme -notin @('http', 'https') -or $musicUri.IsLoopback) {
    throw 'MusicUiUrl must use HTTP or HTTPS and resolve to the host computer from the PS5, not loopback.'
}
$MusicUiUrl = $musicUri.AbsoluteUri
if ($MusicUiUrl.Contains("'")) {
    throw 'MusicUiUrl cannot contain an apostrophe.'
}

$null = Get-Command mingw32-make.exe -ErrorAction Stop
$compiler = Get-Command g++.exe -ErrorAction Stop
$versionOutput = & $clang --version | Select-Object -First 1
$versionMatch = [regex]::Match($versionOutput, 'clang version (\d+\.\d+\.\d+)')
if (-not $versionMatch.Success) {
    throw "Could not read the LLVM version from '$versionOutput'"
}
$llvmVersion = $versionMatch.Groups[1].Value
$llvmMajor = $llvmVersion.Split('.')[0]
if ([int]$llvmMajor -lt 18 -or [int]$llvmMajor -gt 23) {
    throw "This SDK helper supports LLVM 18 through 23; found LLVM $llvmVersion."
}

$shimDirectory = Join-Path $env:LOCALAPPDATA 'PS5-SACD\toolchain-shims'
New-Item -ItemType Directory -Force -Path $shimDirectory | Out-Null
$llvmConfig = Join-Path $shimDirectory "llvm-config-$llvmMajor"
$llvmConfigScript = @'
#!/usr/bin/env bash
case "${1:-}" in
  --version) printf '%s\n' "$PS5_LLVM_VERSION" ;;
  --bindir) cygpath -u "$PS5_LLVM_BIN" ;;
  *) printf 'Unsupported llvm-config query on Windows: %s\n' "${1:-}" >&2; exit 1 ;;
esac
'@ -replace "`r`n", "`n"
Set-Content -LiteralPath $llvmConfig -Value $llvmConfigScript -Encoding ascii -NoNewline

$linkerScript = Join-Path $SdkPath 'bin\prospero-lld'
$linkerShellScript = "$linkerScript.sh"
if ((Test-Path -LiteralPath $linkerScript) -and -not (Test-Path -LiteralPath $linkerShellScript)) {
    Move-Item -LiteralPath $linkerScript -Destination $linkerShellScript
}
if (-not (Test-Path -LiteralPath $linkerShellScript)) {
    throw "The SDK linker script was not found at $linkerShellScript"
}

$proxySource = Join-Path $PSScriptRoot 'windows\prospero_lld_proxy.cpp'
$proxyOutput = Join-Path $SdkPath 'bin\prospero-lld.exe'
& $compiler.Source -std=c++17 -O2 -municode $proxySource -o $proxyOutput
if ($LASTEXITCODE -ne 0) {
    throw 'Could not build the Windows linker bridge.'
}

$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$env:PS5_PAYLOAD_SDK = $SdkPath
$env:PS5_GIT_BASH = $GitBash
$env:PS5_LLVM_BIN = $LlvmBin
$env:PS5_LLVM_VERSION = $llvmVersion
$env:PS5_LLVM_CONFIG_PATH = $llvmConfig
$env:PS5_SACD_ROOT = $repoRoot
$env:PS5_MUSIC_UI_URL = $MusicUiUrl
$env:PATH = "$($SdkPath)\bin;$shimDirectory;$LlvmBin;$(Split-Path -Parent $GitBash);$env:PATH"

& $GitBash -c 'chmod +x "$(cygpath -u "$PS5_LLVM_CONFIG_PATH")"; ROOT="$(cygpath -u "$PS5_SACD_ROOT")"; cd "$ROOT" && mingw32-make -f Makefile.ps5'
if ($LASTEXITCODE -ne 0) {
    throw "PS5 payload build failed (mingw32-make exit $LASTEXITCODE)."
}

Write-Output "Built $repoRoot\out\build\ps5\dsf_info.elf and music_ui.elf using LLVM $llvmVersion."
