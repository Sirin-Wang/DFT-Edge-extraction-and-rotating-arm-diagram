$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $root

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
Write-Host "The server will choose 5174, or the next free port if 5174 is busy."
Write-Host "Keep this window open while using the viewer."
Write-Host ""

node dft_static_server.mjs 5174 --open

Write-Host ""
Write-Host "Viewer server stopped."
Read-Host "Press Enter to close"
