@echo off
gcc -Wall -O2 -o src\membomb.exe src\membomb.c
if errorlevel 1 exit /b 1
gcc -Wall -O2 -DMEMBOMB_RAW -o src\membomb_raw.exe src\membomb.c
if errorlevel 1 exit /b 1
echo Built src\membomb.exe and src\membomb_raw.exe
