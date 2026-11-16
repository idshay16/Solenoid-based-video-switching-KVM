<#
.SYNOPSIS
  Build and run the host-side unit tests (firmware/tests) with g++ in WSL.
.EXAMPLE
  .\scripts\test.ps1
#>
$Repo = Split-Path -Parent $PSScriptRoot
wsl --cd "$Repo" sh scripts/test.sh
exit $LASTEXITCODE
