@echo off
cd /d "%~dp0"

rem Usage examples:
rem   export_dft_scene_animations.cmd art XDoG_Guide
rem   export_dft_scene_animations.cmd art2 XDoG_Support 1024 128 90 simultaneous 5 32
rem   export_dft_scene_animations.cmd art XDoG_Guide 2048 320 90 sequential 3 24 true

set IMAGE=%~1
set CHANNEL=%~2
set SAMPLES=%~3
set ARMS=%~4
set DURATION=%~5
set MODE=%~6
set HOLD=%~7
set SIM_ARM_PARTS=%~8
set FIXED_CENTER=%~9

if "%IMAGE%"=="" set IMAGE=art
if "%CHANNEL%"=="" set CHANNEL=XDoG_Guide

set ARGS=--image=%IMAGE% --channel=%CHANNEL%
if not "%SAMPLES%"=="" set ARGS=%ARGS% --samples=%SAMPLES%
if not "%ARMS%"=="" set ARGS=%ARGS% --arms=%ARMS%
if not "%DURATION%"=="" set ARGS=%ARGS% --duration=%DURATION%
if not "%MODE%"=="" set ARGS=%ARGS% --mode=%MODE%
if not "%HOLD%"=="" set ARGS=%ARGS% --hold=%HOLD%
if not "%SIM_ARM_PARTS%"=="" set ARGS=%ARGS% --sim-arm-parts=%SIM_ARM_PARTS%
if not "%FIXED_CENTER%"=="" set ARGS=%ARGS% --fixed-center=%FIXED_CENTER%

node export_dft_scene_animation.mjs %ARGS%
