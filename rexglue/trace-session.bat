@echo off
rem Gameplay trace for frame-pacing analysis: logs a timeline and the main thread's stacks around slow frames.
cd /d "%~dp0"
start "" pacman.exe --pacman_frame_trace=true --pacman_profile=true --log_file=pacman-trace.log
