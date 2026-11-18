@echo off
title COMPILADOR Y CARGADOR ESP32-P4 (ESP-IDF v5.3.5)
color 0A
echo =====================================================================
echo    COMPILANDO Y FLASHEANDO FIRMWARE EN ESP32-P4 (MIPI-CSI CAM)
echo    Proyecto Final de Inteligencia Artificial (2026-2)
echo =====================================================================
echo.

set "IDF_TOOLS_PATH=C:\Espressif"
set "IDF_PATH=C:\Espressif\frameworks\esp-idf-v5.3.5"
set "IDF_PYTHON_ENV_PATH=C:\Espressif\python_env\idf5.3_py3.11_env"

echo [1/3] Activando entorno ESP-IDF v5.3.5...
call "C:\Espressif\frameworks\esp-idf-v5.3.5\export.bat"
if %errorlevel% neq 0 (
    echo [ERROR] No se pudo activar el entorno ESP-IDF.
    pause
    exit /b %errorlevel%
)

cd /d "E:\Universidad\IA\ProyectoPollito\Proyecto_Final_IANico\Proyecto_Final_IA\esp-dl\examples\obstacle_person_detect"

echo.
echo [2/3] Compilando con directorio de longitud segura (C:\build_p4)...
idf.py -B C:\build_p4 build
if %errorlevel% neq 0 (
    echo [ERROR] Fallo la compilacion.
    pause
    exit /b %errorlevel%
)

echo.
echo [3/3] Flasheando firmware al ESP32-P4 en puerto COM5...
C:\Espressif\python_env\idf5.3_py3.11_env\Scripts\python.exe -m esptool --chip esp32p4 -b 460800 --port COM5 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_size 16MB --flash_freq 80m 0x10000 C:\build_p4\obstacle_person_detect.bin

echo.
echo =====================================================================
echo [EXITO] Firmware cargado correctamente en tu ESP32-P4.
echo Puedes abrir INICIAR_DASHBOARD.bat para ver la telemetria en vivo.
echo =====================================================================
pause
