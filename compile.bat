@echo off
gcc -Wall -Wextra -O2 -o src\membomb.exe src\membomb.c
if errorlevel 1 exit /b 1
echo Built src\membomb.exe
