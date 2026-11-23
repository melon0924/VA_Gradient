param(
    [string]$BuildToolsRoot,
    [switch]$SkipTests
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
if (-not $BuildToolsRoot) {
    $BuildToolsRoot = Join-Path (Split-Path -Parent $projectRoot) 'tools\vs-buildtools'
}
$vcvars = Join-Path $BuildToolsRoot 'VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path -LiteralPath $vcvars)) {
    throw "Visual Studio environment script was not found: $vcvars"
}
if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    throw 'CMake is required on PATH.'
}

$buildRoot = Join-Path $projectRoot 'build'
New-Item -ItemType Directory -Force -Path $buildRoot | Out-Null
$batch = Join-Path $buildRoot ("configure-{0}.cmd" -f [guid]::NewGuid().ToString('N'))
$commands = @(
    '@echo off'
    ('call "{0}"' -f $vcvars)
    'if errorlevel 1 exit /b %errorlevel%'
    'cmake --preset windows-msvc'
    'if errorlevel 1 exit /b %errorlevel%'
    'cmake --build --preset windows-msvc'
    'if errorlevel 1 exit /b %errorlevel%'
)
if (-not $SkipTests) {
    $commands += @(
        'ctest --preset windows-msvc'
        'if errorlevel 1 exit /b %errorlevel%'
    )
}
$commands += 'exit /b 0'
Set-Content -LiteralPath $batch -Value $commands -Encoding ASCII
Push-Location -LiteralPath $projectRoot
try {
    & cmd.exe /d /c $batch
    if ($LASTEXITCODE -ne 0) {
        throw "Build or tests failed with exit code $LASTEXITCODE."
    }
} finally {
    Pop-Location
    Remove-Item -LiteralPath $batch
}

