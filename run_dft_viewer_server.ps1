param(
    [int]$Port = 5174
)

$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $root

$localEnv = Join-Path $root "local_environment.ps1"
if (Test-Path -LiteralPath $localEnv) {
    . $localEnv
}

Write-Host "ProjectFourier DFT viewer"
Write-Host "Root: $root"

$node = Get-Command node -ErrorAction SilentlyContinue
if (-not $node) {
    Write-Host ""
    Write-Host "Node.js was not found in PATH. Install Node.js or add node.exe to PATH."
    Read-Host "Press Enter to close"
    exit 1
}

Write-Host "Node: $($node.Source)"
Write-Host ""
Write-Host "Starting local viewer server..."
Write-Host "The server will choose $Port, or the next free port if $Port is busy."
Write-Host "Keep this window open while using the viewer."
Write-Host ""

node dft_static_server.mjs $Port --open

Write-Host ""
Write-Host "Viewer server stopped."
Read-Host "Press Enter to close"
