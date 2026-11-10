# Native C++ build entry point. Parameters are forwarded to scripts/build.ps1.
& (Join-Path $PSScriptRoot 'scripts\build.ps1') @args
exit $LASTEXITCODE
