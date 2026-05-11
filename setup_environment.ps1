[CmdletBinding()]
param(
    [switch]$InstallMissing,
    [switch]$SkipBuild,
    [switch]$StartViewer,
    [string]$OpenCVDir = "",
    [string]$PotraceDir = "",
    [int]$Port = 5174
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $root

$script:issues = New-Object System.Collections.Generic.List[string]
$script:warnings = New-Object System.Collections.Generic.List[string]

function Write-Section([string]$Text) {
    Write-Host ""
    Write-Host "== $Text =="
}

function Add-Issue([string]$Text) {
    $script:issues.Add($Text) | Out-Null
    Write-Host "[missing] $Text" -ForegroundColor Yellow
}

function Add-Warning([string]$Text) {
    $script:warnings.Add($Text) | Out-Null
    Write-Host "[warn] $Text" -ForegroundColor Yellow
}

function Add-PathEntry([string]$PathItem) {
    if (-not $PathItem -or -not (Test-Path -LiteralPath $PathItem)) { return }
    $resolved = (Resolve-Path -LiteralPath $PathItem).Path
    $items = $env:PATH -split ";"
    if ($items -notcontains $resolved) {
        $env:PATH = "$resolved;$env:PATH"
    }
}

function Quote-PS([string]$Text) {
    return "'" + ($Text -replace "'", "''") + "'"
}

function Get-ExistingPath([string[]]$Candidates) {
    foreach ($candidate in ($Candidates | Where-Object { $_ } | Select-Object -Unique)) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }
    return ""
}

function Find-MSBuild {
    $cmd = Get-Command msbuild.exe -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($cmd) { return $cmd.Source }

    $vswhereCandidates = @(
        (Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"),
        (Join-Path $env:ProgramFiles "Microsoft Visual Studio\Installer\vswhere.exe")
    ) | Where-Object { $_ -and (Test-Path -LiteralPath $_) }

    foreach ($vswhere in $vswhereCandidates) {
        $found = & $vswhere -latest -products "*" -requires Microsoft.Component.MSBuild -find "MSBuild\**\Bin\MSBuild.exe" 2>$null | Select-Object -First 1
        if ($found -and (Test-Path -LiteralPath $found)) { return $found }
    }

    $roots = @(
        (Join-Path $env:ProgramFiles "Microsoft Visual Studio"),
        (Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio")
    ) | Where-Object { $_ -and (Test-Path -LiteralPath $_) }
    $versions = @("18", "2022", "17", "2019")
    $editions = @("Community", "Professional", "Enterprise", "BuildTools")

    foreach ($base in $roots) {
        foreach ($version in $versions) {
            foreach ($edition in $editions) {
                $path = Join-Path $base "$version\$edition\MSBuild\Current\Bin\MSBuild.exe"
                if (Test-Path -LiteralPath $path) { return $path }
            }
        }
    }

    return ""
}

function Test-OpenCVRoot([string]$Dir) {
    if (-not $Dir -or -not (Test-Path -LiteralPath $Dir)) { return $false }
    $include = Join-Path $Dir "include\opencv2\opencv.hpp"
    $lib = Join-Path $Dir "x64\vc16\lib\opencv_world4120.lib"
    return (Test-Path -LiteralPath $include) -and (Test-Path -LiteralPath $lib)
}

function Find-OpenCV {
    foreach ($candidate in @(
        $OpenCVDir,
        $env:OPENCV_DIR,
        (Join-Path $root "deps\opencv\build"),
        (Join-Path $root "tools\opencv\build"),
        "C:\opencv\build",
        "C:\tools\opencv\build",
        (Join-Path $HOME "Documents\opencv\build"),
        (Join-Path $HOME "Documents\openCV\opencv\build")
    ) | Where-Object { $_ } | Select-Object -Unique) {
        if (Test-OpenCVRoot $candidate) {
            $resolved = (Resolve-Path -LiteralPath $candidate).Path
            $bin = Join-Path $resolved "x64\vc16\bin"
            $dll = Join-Path $bin "opencv_world4120.dll"
            return [pscustomobject]@{
                Found = $true
                Root = $resolved
                Bin = $bin
                Dll = $(if (Test-Path -LiteralPath $dll) { $dll } else { "" })
            }
        }
    }

    return [pscustomobject]@{ Found = $false; Root = ""; Bin = ""; Dll = "" }
}

function Resolve-PotraceCandidate([string]$Candidate) {
    if (-not $Candidate) { return "" }
    return Get-ExistingPath @(
        $(if ((Split-Path -Leaf $Candidate) -ieq "potrace.exe") { $Candidate }),
        (Join-Path $Candidate "potrace.exe")
    )
}

function Find-Potrace {
    foreach ($candidate in @(
        $PotraceDir,
        $env:POTRACE_DIR,
        (Join-Path $root "tools\potrace"),
        (Join-Path $root "deps\potrace"),
        "C:\potrace",
        "C:\tools\potrace",
        (Join-Path $HOME "Documents\POTRACE\potrace-1.16.win64")
    ) | Where-Object { $_ } | Select-Object -Unique) {
        $exe = Resolve-PotraceCandidate $candidate
        if ($exe) { return $exe }
    }

    $cmd = Get-Command potrace.exe -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($cmd) { return $cmd.Source }
    return ""
}

function Write-LocalEnvironment($OpenCV, [string]$PotraceExe) {
    $path = Join-Path $root "local_environment.ps1"
    $lines = @(
        "# Generated by setup_environment.ps1. This file is machine-local.",
        "`$env:PROJECTFOURIER_ROOT = $(Quote-PS $root)",
        "function Add-ProjectFourierPath([string]`$PathItem) {",
        "    if (`$PathItem -and (Test-Path -LiteralPath `$PathItem) -and ((`$env:PATH -split ';') -notcontains `$PathItem)) {",
        "        `$env:PATH = `"`$PathItem;`$env:PATH`"",
        "    }",
        "}"
    )

    if ($OpenCV.Found) {
        $lines += "`$env:OPENCV_DIR = $(Quote-PS $OpenCV.Root)"
        $lines += "Add-ProjectFourierPath $(Quote-PS $OpenCV.Bin)"
    }
    if ($PotraceExe) {
        $lines += "Add-ProjectFourierPath $(Quote-PS (Split-Path -Parent $PotraceExe))"
    }

    Set-Content -LiteralPath $path -Encoding UTF8 -Value $lines
    return $path
}

function Build-Project([string]$MSBuild, [string]$ProjectFile) {
    Write-Host "Building $ProjectFile ..."
    & $MSBuild $ProjectFile /p:Configuration=Release /p:Platform=x64 /m
    if ($LASTEXITCODE -ne 0) {
        throw "Build failed: $ProjectFile"
    }
}

Write-Host "ProjectFourier environment setup"
Write-Host "Root: $root"

Write-Section "Tools"

$node = Get-Command node.exe -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $node -and $InstallMissing) {
    $winget = Get-Command winget.exe -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($winget) {
        Write-Host "Installing Node.js LTS with winget..."
        & $winget.Source install --id OpenJS.NodeJS.LTS -e --accept-package-agreements --accept-source-agreements
        $node = Get-Command node.exe -ErrorAction SilentlyContinue | Select-Object -First 1
    } else {
        Add-Warning "winget was not found; Node.js cannot be installed automatically."
    }
}
if ($node) { Write-Host "[ok] Node.js: $($node.Source)" }
else { Add-Issue "Node.js was not found. Install Node.js LTS and rerun this script." }

$msbuild = Find-MSBuild
if ($msbuild) {
    Write-Host "[ok] MSBuild: $msbuild"
} else {
    Add-Issue "MSBuild was not found. Install Visual Studio or Build Tools with Desktop development with C++."
}

$opencv = Find-OpenCV
if ($opencv.Found) {
    Write-Host "[ok] OpenCV: $($opencv.Root)"
    Add-PathEntry $opencv.Bin
    $env:OPENCV_DIR = $opencv.Root
    if (-not $opencv.Dll) {
        Add-Warning "OpenCV runtime DLL opencv_world4120.dll was not found under $($opencv.Bin)."
    }
} else {
    Add-Issue "OpenCV 4.12.0 build folder was not found. Set OPENCV_DIR or rerun with -OpenCVDir <path-to-opencv-build>."
}

$potrace = Find-Potrace
if ($potrace) {
    Write-Host "[ok] potrace: $potrace"
    Add-PathEntry (Split-Path -Parent $potrace)
} else {
    Add-Issue "potrace.exe was not found. Install potrace and add it to PATH, or rerun with -PotraceDir <folder>."
}

$localEnv = Write-LocalEnvironment $opencv $potrace
Write-Host "[ok] Local environment file: $localEnv"

Write-Section "Build"
if ($SkipBuild) {
    Write-Host "Build skipped by -SkipBuild."
} elseif (-not $msbuild) {
    Add-Warning "Build skipped because MSBuild is missing."
} else {
    Build-Project $msbuild "DftPrecompute.vcxproj"
    if ($opencv.Found) {
        Build-Project $msbuild "ProjectFourier.vcxproj"
    } else {
        Add-Warning "ProjectFourier build skipped because OpenCV is missing."
    }

    if ($opencv.Dll -and (Test-Path -LiteralPath (Join-Path $root "x64\Release"))) {
        Copy-Item -LiteralPath $opencv.Dll -Destination (Join-Path $root "x64\Release") -Force
        Write-Host "[ok] Copied OpenCV runtime DLL to x64\Release"
    }
}

Write-Section "Result"
$extractExe = Join-Path $root "x64\Release\ProjectFourier.exe"
$precomputeExe = Join-Path $root "x64\Release\DftPrecompute.exe"
if (Test-Path -LiteralPath $extractExe) { Write-Host "[ok] $extractExe" } else { Add-Warning "ProjectFourier.exe is not available yet." }
if (Test-Path -LiteralPath $precomputeExe) { Write-Host "[ok] $precomputeExe" } else { Add-Warning "DftPrecompute.exe is not available yet." }

if ($script:issues.Count -gt 0) {
    Write-Host ""
    Write-Host "Setup finished with missing dependencies:" -ForegroundColor Yellow
    foreach ($issue in $script:issues) { Write-Host " - $issue" -ForegroundColor Yellow }
    Write-Host ""
    Write-Host "After installing the missing tools, run setup_environment.cmd again."
} else {
    Write-Host ""
    Write-Host "Setup complete."
    Write-Host "Start the viewer with: start_dft_viewer.cmd"
}

if ($StartViewer) {
    Write-Section "Viewer"
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root "run_dft_viewer_server.ps1") -Port $Port
}
