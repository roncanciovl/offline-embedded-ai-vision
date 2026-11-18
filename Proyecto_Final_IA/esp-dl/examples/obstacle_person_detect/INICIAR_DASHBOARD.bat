@echo off
title DASHBOARD DE IA - ESP32-P4 (ABET 2026-2)
color 0A
echo =====================================================================
echo    SISTEMA EMBEBIDO DE DETECCION DE OBSTACULOS Y PERSONAS (ESP32-P4)
echo    Proyecto Final de Inteligencia Artificial - 3er Corte
echo =====================================================================
echo.
echo Iniciando comunicacion serial y panel de metricas en tiempo real...
echo (Presiona 'S' para capturar evidencia, 'ESPACIO' para pausar, 'Q' para salir)
echo.

python dashboard_viewer.py

if %errorlevel% neq 0 (
    echo.
    echo Ocurrio un error al ejecutar el dashboard.
    pause
)

