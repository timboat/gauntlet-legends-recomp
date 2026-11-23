@echo off
rem Builds the PS Vita version (vita_files\GauntletLegends.vpk + the ROM laid out for the memory card).
rem Runs SETUP.bat's steps that are still needed first, so it also works on a fresh copy.
call "%~dp0SETUP.bat" --vita-only %*
