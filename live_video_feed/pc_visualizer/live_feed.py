import serial
import json
import base64
import cv2
import numpy as np
import time

# Configure your serial port
SERIAL_PORT = 'COM4'
BAUD_RATE = 115200

# Classes based on the model training
CLASSES = {
    0: "Obstacle",
    1: "Person"
}
COLORS = {
    0: (0, 0, 255),    # Red for obstacle
    1: (0, 255, 0)     # Green for person
}

def main():
    print(f"Connecting to {SERIAL_PORT} at {BAUD_RATE} baud...")
    try:
        ser = serial.Serial()
        ser.port = SERIAL_PORT
        ser.baudrate = BAUD_RATE
        ser.timeout = 1
        ser.dtr = False
        ser.rts = False
        ser.open()
        
        # Reset the board to ensure it starts fresh
        ser.dtr = False
        ser.rts = True
        time.sleep(0.1)
        ser.dtr = False
        ser.rts = False
        time.sleep(0.5)
        
    except Exception as e:
        print(f"Failed to open {SERIAL_PORT}: {e}")
        return

    buffer = ""
    in_json = False

    cv2.namedWindow("ESP32-P4 Live Feed", cv2.WINDOW_NORMAL)
    cv2.resizeWindow("ESP32-P4 Live Feed", 448, 448) # upscale 2x for better viewing

    print("Listening for video and detection stream...")

    while True:
        try:
            line = ser.readline().decode('utf-8', errors='ignore').strip()
        except serial.SerialException:
            break
        
        if not line:
            # Must call waitKey to keep window responsive even when no data
            if cv2.getWindowProperty("ESP32-P4 Live Feed", cv2.WND_PROP_VISIBLE) >= 0:
                if cv2.waitKey(1) & 0xFF == ord('q'):
                    break
            continue
            
        if line == ">>>JSON_START<<<":
            in_json = True
            buffer = ""
            continue
            
        if line == ">>>JSON_END<<<":
            in_json = False
            try:
                data = json.loads(buffer)
                
                # 1. Decode Image
                b64_str = data.get("image", "")
                if b64_str:
                    img_data = base64.b64decode(b64_str)
                    np_arr = np.frombuffer(img_data, np.uint8)
                    frame = cv2.imdecode(np_arr, cv2.IMREAD_COLOR)
                else:
                    # Fallback blank frame
                    frame = np.zeros((224, 224, 3), dtype=np.uint8)
                
                # 2. Draw Detections
                detections = data.get("detections", [])
                for det in detections:
                    cat = det["category"]
                    score = det["score"]
                    box = det["box"] # [x1, y1, x2, y2]
                    
                    label = f"{CLASSES.get(cat, 'Unknown')}: {score:.2f}"
                    color = COLORS.get(cat, (255, 255, 255))
                    
                    # Draw Box
                    cv2.rectangle(frame, (box[0], box[1]), (box[2], box[3]), color, 2)
                    
                    # Draw Label
                    cv2.putText(frame, label, (box[0], max(0, box[1] - 10)), 
                                cv2.FONT_HERSHEY_SIMPLEX, 0.5, color, 1)

                # 3. Display
                if frame is not None:
                    cv2.imshow("ESP32-P4 Live Feed", frame)
                    if cv2.waitKey(1) & 0xFF == ord('q'):
                        break

            except json.JSONDecodeError:
                print("JSON Decode Error! Buffer contents:")
                print(buffer)
            except Exception as e:
                print(f"Error processing frame: {e}")
                
            continue

        if in_json:
            buffer += line
        else:
            # Print board debug logs so we can see if it's crashing!
            print(f"BOARD: {line}")
            if cv2.getWindowProperty("ESP32-P4 Live Feed", cv2.WND_PROP_VISIBLE) >= 0:
                if cv2.waitKey(1) & 0xFF == ord('q'):
                    break

    ser.close()
    cv2.destroyAllWindows()

if __name__ == "__main__":
    main()
