import base64
import json
import os
import sys

import cv2
import numpy as np
import serial
import serial.tools.list_ports

# Folder with the original images (same files flashed to SPIFFS).
# Resolved relative to this script, so it works from any working directory
# and on Windows, Linux and macOS.
IMAGE_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "main", "images")

# app_main.cpp switches UART0 to 921600 baud for the live camera telemetry.
# Override with a second argument: `python pc_viewer.py COM3 115200`
BAUDRATE = int(sys.argv[2]) if len(sys.argv) > 2 else 921600
CLASS_NAMES = {0: "obstacle", 1: "person"}
CLASS_COLORS = {0: (0, 165, 255), 1: (0, 255, 0)}  # BGR: obstacle = orange, person = green


def find_esp32_port():
    """Return the serial port to use, or None if there is none."""
    # Optional: pass the port as an argument, e.g. `python pc_viewer.py COM3`
    if len(sys.argv) > 1:
        return sys.argv[1]

    ports = serial.tools.list_ports.comports()
    if not ports:
        return None
    if len(ports) == 1:
        return ports[0].device

    print("Available serial ports:")
    for p in ports:
        print(f" - {p.device}: {p.description}")
    port = input("Enter your ESP32 port (e.g. COM3 or /dev/ttyACM0): ").strip()
    return port or None


def clamp(value, low, high):
    return max(low, min(int(value), high))


def process_image(data):
    filename = data.get("image")
    detections = data.get("detections", [])

    if not filename:
        return

    if data.get("image_b64"):
        # Live camera frame: the ESP32 sends a JPEG thumbnail; boxes are in full-frame coordinates.
        img = cv2.imdecode(np.frombuffer(base64.b64decode(data["image_b64"]), np.uint8), cv2.IMREAD_COLOR)
        if img is None:
            print("Failed to decode camera thumbnail")
            return
        sx = img.shape[1] / data.get("width", img.shape[1])
        sy = img.shape[0] / data.get("height", img.shape[0])
        detections = [dict(d, box=[d["box"][0] * sx, d["box"][1] * sy, d["box"][2] * sx, d["box"][3] * sy])
                      for d in detections]
        draw_and_show(img, f"camera frame {data.get('frame', '?')}", data, detections, wait=1)
        return

    img_path = os.path.join(IMAGE_DIR, filename)
    if not os.path.exists(img_path):
        print(f"Image not found on PC: {img_path}")
        return

    img = cv2.imread(img_path)
    if img is None:
        print(f"Failed to load image: {img_path}")
        return
    draw_and_show(img, filename, data, detections, wait=0)


def draw_and_show(img, label_name, data, detections, wait):
    """Draw boxes, print a summary and show the window. wait=0 blocks until a key is pressed."""
    timing = ""
    if "inference_ms" in data:
        timing = f" (decode {data.get('decode_ms', '-')} ms, inference {data['inference_ms']} ms)"
    print(f"\nAnalyzed {label_name}: found {len(detections)} objects{timing}")
    h, w = img.shape[:2]

    for det in detections:
        cat = det["category"]
        score = det["score"]
        # ESP-DL boxes are [x1, y1, x2, y2] in original image coordinates
        x1, y1, x2, y2 = det["box"]
        x1, x2 = clamp(x1, 0, w - 1), clamp(x2, 0, w - 1)
        y1, y2 = clamp(y1, 0, h - 1), clamp(y2, 0, h - 1)

        color = CLASS_COLORS.get(cat, (255, 255, 255))
        class_name = CLASS_NAMES.get(cat, f"class {cat}")
        print(f"  - {class_name} {score:.2f} [{x1}, {y1}, {x2}, {y2}]")

        cv2.rectangle(img, (x1, y1), (x2, y2), color, 2)
        label = f"{class_name} ({score:.2f})"
        cv2.putText(img, label, (x1, max(15, y1 - 10)), cv2.FONT_HERSHEY_SIMPLEX, 0.5, color, 2)

    cv2.imshow("ESP32 Detection Output", img)
    if wait == 0:
        print(">>> Press any key on the image window to continue, or 'q' to quit.")

    key = cv2.waitKey(wait) & 0xFF
    if key == ord("q"):
        cv2.destroyAllWindows()
        sys.exit(0)


def main():
    port = find_esp32_port()
    if not port:
        print("No serial port found. Connect the ESP32-P4 via USB and try again.")
        return

    try:
        ser = serial.Serial(port, BAUDRATE, timeout=1)
    except serial.SerialException as e:
        print(f"Error opening serial port {port}: {e}")
        print("Make sure you closed the ESP-IDF monitor (only one program can use the port).")
        return

    print(f"Connected to {port} at {BAUDRATE} baud.")
    print("Waiting for ESP32 output... (Make sure to close other serial monitors!)")

    json_str = ""
    in_json = False

    try:
        while True:
            try:
                line = ser.readline().decode("utf-8", errors="ignore").strip()
            except serial.SerialException as e:
                print(f"Serial error: {e}")
                break

            if not line:
                continue

            if line == ">>>JSON_START<<<":
                in_json = True
                json_str = ""
                continue

            if line == ">>>JSON_END<<<":
                in_json = False
                try:
                    data = json.loads(json_str)
                except json.JSONDecodeError as e:
                    print(f"Failed to parse JSON: {e}")
                    print(f"Raw string: {json_str}")
                    continue
                process_image(data)
                continue

            if in_json:
                json_str += line + "\n"
            else:
                # Print standard ESP32 logs
                print(f"[ESP32] {line}")
    except KeyboardInterrupt:
        pass
    finally:
        ser.close()
        cv2.destroyAllWindows()


if __name__ == "__main__":
    main()
