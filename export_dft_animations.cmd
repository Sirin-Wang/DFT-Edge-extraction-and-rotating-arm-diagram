@echo off
cd /d "%~dp0"

rem Usage examples:
rem   export_dft_animations.cmd art XDoG_Guide 0 2048 256 8 3
rem   export_dft_animations.cmd art2 XDoG_Support 42 1024 128 10 5
rem   export_dft_animations.cmd art XDoG_Guide 0 4096 512 12 3 full single true
rem   export_dft_animations.cmd art3 XDoG_Guide 0 128 1024 12 3 full single true true 16384

set IMAGE=%~1
set CHANNEL=%~2
set COMPONENT=%~3
set SAMPLES=%~4
set ARMS=%~5
set DURATION=%~6
set HOLD=%~7
set SOURCE=%~8
set DFT_MODE=%~9
shift
set FIXED_CENTER=%~9
shift
set FULL_SAMPLES_PER_COMPONENT=%~9
shift
set MAX_FULL_SAMPLES=%~9

if "%IMAGE%"=="" set IMAGE=art
if "%CHANNEL%"=="" set CHANNEL=XDoG_Guide
if "%COMPONENT%"=="" set COMPONENT=0
if "%SAMPLES%"=="" set SAMPLES=2048
if "%ARMS%"=="" set ARMS=256
if "%DURATION%"=="" set DURATION=8
if "%HOLD%"=="" set HOLD=3

set ARGS=--image=%IMAGE% --channel=%CHANNEL% --component=%COMPONENT% --samples=%SAMPLES% --arms=%ARMS% --duration=%DURATION% --hold=%HOLD%
if not "%SOURCE%"=="" set ARGS=%ARGS% --source=%SOURCE%
if not "%DFT_MODE%"=="" set ARGS=%ARGS% --dft-mode=%DFT_MODE%
if not "%FIXED_CENTER%"=="" set ARGS=%ARGS% --fixed-center=%FIXED_CENTER%
if not "%FULL_SAMPLES_PER_COMPONENT%"=="" set ARGS=%ARGS% --full-samples-per-component=%FULL_SAMPLES_PER_COMPONENT%
if not "%MAX_FULL_SAMPLES%"=="" set ARGS=%ARGS% --max-full-samples=%MAX_FULL_SAMPLES%

node export_dft_animation.mjs %ARGS%
