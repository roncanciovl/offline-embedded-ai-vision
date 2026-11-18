# Proyecto: Live Video Feed - Problemas de Conexión de la Cámara

Este documento explica los principales problemas que encontramos al intentar conectar y hacer funcionar la cámara con la placa ESP32-P4 para obtener un flujo de video en vivo (Live Video Feed). Actualmente, la conexión de la cámara no se ha logrado de manera exitosa. 

Dejamos este registro para que los futuros desarrolladores que trabajen en este proyecto conozcan las limitaciones y los obstáculos que enfrentamos.

## Problemas Principales Encontrados

### 1. Incompatibilidad y Configuración de Pines (Pinout)
Uno de los mayores desafíos fue encontrar la configuración correcta de los pines. La placa ESP32-P4 maneja interfaces específicas para cámaras (como MIPI CSI o DVP), pero la asignación de pines (Data, Clock, I2C/SCCB) entre el módulo de la cámara y la placa no coincidía de manera directa o no estaba claramente documentada, lo que impedía la correcta comunicación entre ambos.

### 2. Problemas de Inicialización (I2C/SCCB)
Para que la cámara funcione, primero debe ser configurada a través del bus I2C (conocido como SCCB en las cámaras Omnivision). Nos encontramos con errores constantes de "Camera Probe Failed" (Fallo en la detección de la cámara) o tiempos de espera agotados (timeouts) al intentar leer o escribir en los registros de la cámara a través del bus de control.

### 3. Frecuencia del Reloj Maestro (XCLK / MCLK)
La cámara requiere una señal de reloj estable (generalmente de 10MHz a 24MHz) proporcionada por la ESP32. Tuvimos dificultades para generar y enrutar esta señal limpia y estable hacia la cámara. Una señal inestable provoca que la cámara no responda correctamente a los comandos de inicialización o que envíe datos corruptos.

### 4. Estabilidad de la Alimentación (Voltaje/Corriente)
Las cámaras consumen picos de corriente altos, especialmente durante la inicialización y la transmisión de video. Observamos que la alimentación proporcionada directamente por los pines de 3.3V de la placa a veces no era lo suficientemente robusta, causando caídas de tensión (brownouts) y reinicios inesperados en la ESP32, o impidiendo que el sensor de imagen encendiera correctamente.

### 5. Soporte de Drivers en ESP-IDF
El soporte para la placa ESP32-P4 en la biblioteca `esp32-camera` y en el entorno ESP-IDF aún puede ser experimental o requerir ramas específicas del repositorio. Tuvimos problemas de compatibilidad de software donde las funciones de captura de imagen o los drivers subyacentes devolvían errores no documentados.

## Recomendaciones para el Futuro
- **Verificar la versión de ESP-IDF:** Asegurarse de usar la versión correcta (por ejemplo, v5.3 o superior) y verificar si hay actualizaciones recientes en el componente `esp32-camera` con mejor soporte para ESP32-P4.
- **Revisar el hardware:** Considerar el uso de una fuente de alimentación externa de 3.3V dedicada exclusivamente para el módulo de la cámara.
- **Analizador Lógico:** Usar un osciloscopio o analizador lógico para confirmar físicamente si las señales I2C y XCLK están llegando correctamente al sensor.

---
*Nota: El resto de los archivos del proyecto que funcionan en otras pruebas se encuentran en las carpetas adjuntas a este directorio.*
