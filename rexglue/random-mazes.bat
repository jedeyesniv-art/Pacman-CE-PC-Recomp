@echo off
rem Championship mode with the maze order shuffled at this launch (the first maze stays).
rem Uses its own save data (userdata\random-mazes): separate high scores, normal scores untouched.
cd /d "%~dp0"
start "" pacman.exe --pacman_random_mazes=true
