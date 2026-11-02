# ESP-DL AI Obstacle & Person Detection Pipeline

This project runs an AI model on the ESP32-P4 to detect obstacles and people from a set of static images, and visualizes the results on your PC via a Python script.

Este proyecto implementa una red neuronal en el ESP32-P4 para detectar obstaculos y personas en imagenes estaticas, y muestra los resultados en la PC.

## Prerequisites

1. **ESP-IDF v5.3+:** You must have ESP-IDF v5.3 or newer installed on your machine to build and flash the firmware. / Se debe descargar el ESP-IDF v5.3 desde la pagina web de Espressif, y seguir los pasos de instalacion.
2. **Python:** You need Python installed along with the `opencv-python` and `pyserial` libraries. / Se debe descargar Python desde la pagina web de Python, y seguir los pasos de instalacion.
3. **Hardware:** An ESP32-P4 board connected to your PC via USB. / Se debe conectar el ESP32-P4 a la PC mediante un cable USB.

## Installation & Setup

### 1. Install Python Dependencies
Open a standard Command Prompt or PowerShell and install the required Python libraries for the PC viewer script / abrir un CMD y ejecutar el siguiente comando:
```cmd
pip install opencv-python pyserial
```

### 2. Build and Flash the ESP32
Open your **ESP-IDF 5.3 PowerShell Environment**, navigate to the specific example folder, set the target, and flash the board / abrir el entorno de ESP-IDF 5.3 powershell, navegar al archivo "example", luego "set target" y "flashear" la tarjeta:
```powershell
# Navigate to the project example folder
cd examples\obstacle_person_detect

# Set the target to ESP32-P4
idf.py set-target esp32p4

# Build the project and flash it to the board
# This will also flash the 1MB SPIFFS partition containing the test images
idf.py build flash
```

*Note: The first time you build, it will automatically download the required components (like the board BSP and `esp_new_jpeg`) from the Espressif Component Registry.*

*Nota: La primera vez que compilas, se descargaran automaticamente los componentes requeridos (como el BSP de la placa y `esp_new_jpeg`) desde el Espressif Component Registry.*

### 3. Run the PC Viewer Pipeline
**CRITICAL:** Close the ESP-IDF terminal or ensure the ESP-IDF monitor is not running. Only one program can listen to the USB port at a time.

**CRITICO:** Cerrar el terminal de ESP-IDF o asegurarse de que el monitor de ESP-IDF no se este ejecutando. Solo un programa puede escuchar el puerto USB a la vez.

Open a standard Command Prompt or PowerShell, navigate to the example folder, and run the viewer script / abrir un CMD y ejecutar el siguiente comando:
```cmd
cd examples\obstacle_person_detect
python pc_viewer.py
```

1. If only one serial port exists it is used automatically; otherwise the script lists them and asks for one (e.g., `COM3` or `/dev/ttyACM0`). You can also pass it as an argument: `python pc_viewer.py COM3` / si solo hay un puerto se usa automaticamente; si hay varios, el script los lista y pide uno (ej: `COM3`). Tambien se puede pasar como argumento.
2. The ESP32 will process the internal images, run the Neural Network inference, and send the bounding box coordinates over the USB cable / La ESP32 procesara las imagenes internas, ejecutara la inferencia de la red neuronal y enviara las coordenadas de las cajas delimitadoras por el cable USB.
3. The Python script will draw the bounding boxes on the original images and pop them up on your screen / El script de Python dibujara las cajas delimitadoras en las imagenes originales y las mostrara en tu pantalla. 
4. **Press any key** on the image window to advance to the next detection / presiona cualquier tecla en la ventana de la imagen para avanzar a la siguiente deteccion.
