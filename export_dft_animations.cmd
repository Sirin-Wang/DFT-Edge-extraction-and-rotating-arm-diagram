@echo off
cd /d "%~dp0"

rem Usage examples:
rem   export_dft_animations.cmd art XDoG_Guide 0 2048 256 8 3
rem   export_dft_animations.cmd art2 XDoG_Support 42 1024 128 10 5

set IMAGE=%~1
set CHANNEL=%~2
set COMPONENT=%~3
set SAMPLES=%~4
set ARMS=%~5
set DURATION=%~6
set HOLD=%~7

if "%IMAGE%"=="" set IMAGE=art
if "%CHANNEL%"=="" set CHANNEL=XDoG_Guide
if "%COMPONENT%"=="" set COMPONENT=0
if "%SAMPLES%"=="" set SAMPLES=2048
if "%ARMS%"=="" set ARMS=256
if "%DURATION%"=="" set DURATION=8
if "%HOLD%"=="" set HOLD=3

node export_dft_animation.mjs --image=%IMAGE% --channel=%CHANNEL% --component=%COMPONENT% --samples=%SAMPLES% --arms=%ARMS% --duration=%DURATION% --hold=%HOLD%
