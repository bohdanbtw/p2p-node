$ErrorActionPreference = 'Stop'
$exe = Join-Path (Split-Path $PSScriptRoot -Parent) 'p2pnode.exe'
$out = Join-Path $PSScriptRoot 'selftest_report.txt'
& $exe --selftest > $out 2>&1
Write-Host "Saved: $out"
