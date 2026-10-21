@echo off
rem Championship mode with the maze order shuffled at this launch (the first maze stays).
cd /d "%~dp0"
start "" pacman.exe --pacman_random_mazes=true
