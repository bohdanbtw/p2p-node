$ErrorActionPreference = 'Stop'
$exe = Join-Path (Split-Path $PSScriptRoot -Parent) 'p2pnode.exe'
& $exe --record-mic (Join-Path $PSScriptRoot 'mic_test.wav') --seconds 10
Write-Host "Saved: $PSScriptRoot\mic_test.wav"
Write-Host "Metrics: $PSScriptRoot\mic_test.wav.json"
