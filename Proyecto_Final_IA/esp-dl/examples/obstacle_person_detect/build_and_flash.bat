@echo off
echo Setting target to ESP32-P4...
idf.py set-target esp32p4
if %errorlevel% neq 0 exit /b %errorlevel%

echo Building the project...
idf.py build
if %errorlevel% neq 0 exit /b %errorlevel%

echo Flashing and starting serial monitor...
idf.py flash monitor
