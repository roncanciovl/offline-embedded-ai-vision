# ESP32-P4 AI Obstacle & Person Detection Pipeline

Este proyecto implementa una red neuronal convolucional para la detección de obstáculos y personas en hardware embebido utilizando el microcontrolador **ESP32-P4** y la biblioteca **ESP-DL** de Espressif. El sistema procesa inferencias a nivel local y transmite los resultados por comunicación serial a una aplicación cliente en Python para su visualización en PC.

---

## Tabla de Contenidos
1. [Estado del Proyecto y Limitación de Hardware](#-estado-del-proyecto-y-limitación-de-hardware-importante)
2. [Estructura del Repositorio](#-estructura-del-repositorio)
3. [Requisitos de Hardware](#-requisitos-de-hardware)
4. [Requisitos de Software](#-requisitos-de-software)
5. [Guía de Compilación y Flasheo](#-guía-de-compilación-y-flasheo)
6. [Visualización en la PC](#-visualización-en-la-pc)
7. [Adquisición del Dataset y Preprocesamiento](#-adquisición-del-dataset-y-preprocesamiento)


---

## Estado del Proyecto y Limitación de Hardware (¡Importante!)

### Contexto del Pipeline de Captura:
El objetivo inicial del proyecto era realizar detección de objetos en tiempo real mediante un flujo de video en vivo capturado directamente con una cámara conectada a los puertos MIPI CSI de la placa de desarrollo.

### Limitación Técnica Encontrada:
La biblioteca de cámara oficial de Espressif (`esp32-camera`) y el SDK actual presentan limitaciones de compatibilidad fuera de la caja para la interfaz **MIPI CSI / CCI** del chip **ESP32-P4** en combinación con la cámara **OV5647 (Raspberry Pi v1.3)** en esta tarjeta específica. Lograr la comunicación nativa de video en tiempo real requiere escribir un controlador de bajo nivel y de sincronización desde cero.

### Solución Implementada (Plan de Contingencia):
Debido a restricciones de tiempo, el pipeline se modificó para funcionar de la siguiente manera:
1. Las imágenes de prueba (JPEG de 224x224) se almacenan en una partición de memoria flash llamada **SPIFFS** en el microcontrolador.
2. El firmware embebido lee las imágenes estáticas de la memoria flash, realiza la decodificación JPEG a RGB888, ejecuta la inferencia neural y transmite las coordenadas calculadas en formato JSON por puerto USB Serial.
3. El visualizador de Python en la PC lee estas coordenadas en tiempo real, las mapea con los archivos originales en el disco local de la PC y dibuja los bounding boxes correspondientes.

---

## Estructura del Repositorio

Para mantener el proyecto limpio y funcional sin alterar las dependencias relativas de ESP-DL, el repositorio está organizado de la siguiente manera:

```text
Proyecto_Final_IA/
├── README.md                                 # Esta documentación
├── .gitignore                                # Exclusiones para Git (evita subir videos y datasets pesados)
├── Proyecto_Final_IA/
│   ├── README.txt                            # Guía rápida bilingüe de compilación y uso
│   └── esp-dl/                               # Biblioteca Deep Learning de Espressif
│       ├── examples/
│       │   └── obstacle_person_detect/       # [CÓDIGO PROPIO] Aplicación de firmware y visualizador PC
│       │       ├── main/                     # Código fuente C++ (app_main.cpp) e imágenes SPIFFS
│       │       ├── partitions.csv            # Tabla de particiones de memoria flash
│       │       └── pc_viewer.py              # Visualizador en Python con OpenCV
│       └── models/
│           └── obstacle_person_detect/       # [MODELO PROPIO] Binario del modelo .espdl y wrapper C++
└── Vision_Carro_Delivery_dataset_adquisition/# Pipeline de adquisición de datos
    ├── vid2frames.m                          # Script de MATLAB para extracción y redimensionamiento
    ├── Delivery_Vision.v4i.yolov8/           # Metadatos del dataset Roboflow (YOLOv8)
    │   └── data.yaml                         # Configuración del dataset (Clases: obstacle, person)
    └── (Excluidos de Git) /raw_images/       # Fotogramas originales extraídos de los videos MP4
```

---

## Requisitos de Hardware

1. **Microcontrolador**: Tarjeta de desarrollo **Guition JC-ESP32P4-M13 DEV** (módulo `JC-ESP32P4-M3` con chip ESP32-P4, 32MB PSRAM y 16MB Flash).
2. **Cámara**: Cámara compatible con Raspberry Pi 5MP V1.3 (Sensor OV5647, cable plano de 15 pines). *Nota: Físicamente conectable al puerto CSI, pero emulada vía flash debido a la limitación de software descrita.*
3. **Accesorios**: Cable de conexión USB-C a PC para flasheo y comunicación serial.

---

## Requisitos de Software

* **Espressif ESP-IDF v5.3 o superior** (el `sdkconfig.defaults.esp32p4` se generó con v5.4): Entorno oficial de desarrollo necesario para compilar y flashear el microcontrolador.
* **Python 3.8+**: Requerido en la PC cliente para ejecutar el visualizador.
* **Dependencias de Python**:
  ```cmd
  pip install opencv-python pyserial
  ```

---

## Guía de Compilación y Flasheo

Siga estos pasos para compilar e instalar el firmware en la tarjeta ESP32-P4:

1. Abra su consola con el entorno **ESP-IDF (v5.3+)** activado.
2. Navegue al directorio de la aplicación ejemplo:
   ```bash
   cd Proyecto_Final_IA/esp-dl/examples/obstacle_person_detect
   ```
3. Defina el chip objetivo a compilar:
   ```bash
   idf.py set-target esp32p4
   ```
4. Compile el proyecto y súbalo a la placa conectada:
   ```bash
   idf.py build flash
   ```
   *(Este comando compilará el código C++, empaquetará las imágenes de la carpeta `main/images` en una partición SPIFFS de 1MB y la flasheará automáticamente).*

---

## Visualización en la PC

Una vez que la tarjeta esté flasheada y ejecutando el bucle de procesamiento:

1. **Cierre cualquier monitor serial** (incluyendo el monitor de VSCode o `idf.py monitor`) para liberar el puerto COM de la tarjeta.
2. Abra una terminal estándar e ingrese al directorio del ejemplo:
   ```bash
   cd Proyecto_Final_IA/esp-dl/examples/obstacle_person_detect
   ```
3. Ejecute el script del visualizador:
   ```bash
   python pc_viewer.py
   ```
4. Si hay un solo puerto serial se usa automáticamente; si hay varios, ingrese el de su tarjeta (ej. `COM3` en Windows o `/dev/ttyACM0` en Linux). También puede pasarlo como argumento: `python pc_viewer.py COM3`.
5. Se abrirá una ventana de OpenCV mostrando las imágenes. En consola se imprimen las detecciones y los tiempos de decodificación e inferencia medidos en el ESP32-P4. **Presione cualquier tecla** en la ventana de la imagen para avanzar al siguiente fotograma detectado. Presione `q` para salir del visualizador.

---

## Adquisición del Dataset y Preprocesamiento

Los datos del proyecto se capturaron y formatearon mediante el siguiente flujo de trabajo:

1. **Adquisición**: Grabación de videos del entorno del carro repartidor en formato MP4 (`Delivery1.mp4` a `Delivery6.mp4`).
2. **Extracción y Redimensionamiento**: El script de MATLAB `vid2frames.m` extrae automáticamente los fotogramas del video y los redimensiona a **224x224 píxeles** (el tamaño esperado por la capa de entrada de la red neuronal).
3. **Etiquetado y Dataset**: Se cargaron los fotogramas redimensionados en **Roboflow** y se etiquetaron bajo dos categorías:
   * `0: obstacle` (Obstáculo en el camino)
   * `1: person` (Personas)
   * Enlace al Dataset en Roboflow Universe: [Dataset Delivery Vision](https://universe.roboflow.com/nicolass-workspace-qeu9g/delivery_vision/dataset/4)
4. **Quantización**: El modelo entrenado en formato YOLOv8 se convirtió y quantizó en un binario compatible con la arquitectura de aceleración de hardware del ESP32-P4 (`models/obstacle_person_detect/models/p4/espdet_pico_224_224_obstacle_person.espdl`, 577 KB).
