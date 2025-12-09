import serial
import time
import re
import os
from datetime import datetime

# Configuration
SERIAL_PORT = 'COM4'  # Change this to your ESP32's serial port
BAUD_RATE = 115200
OUTPUT_FILE = 'inventory_log.txt'

def main():
    print(f"Starting Inventory Monitor on {SERIAL_PORT}...")
    print(f"Saving data to {os.path.abspath(OUTPUT_FILE)}")
    
    try:
        ser = serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=1)
    except serial.SerialException as e:
        print(f"Error opening serial port: {e}")
        print("Please check if the port is correct and not used by another program (like VS Code Monitor).")
        return

    with open(OUTPUT_FILE, 'a', encoding='utf-8') as f:
        f.write(f"\n--- Session Started: {datetime.now()} ---\n")

    try:
        while True:
            if ser.in_waiting > 0:
                try:
                    line_bytes = ser.readline()
                    line = line_bytes.decode('utf-8', errors='replace').strip()
                    
                    # Print to console
                    print(line)
                    
                    # Check for inventory addition logs
                    # Pattern matches: I (71964) inventory: Added item: mei id:1765269159_0001 qty:1 loc: remaining:7
                    if "inventory: Added item:" in line:
                        with open(OUTPUT_FILE, 'a', encoding='utf-8') as f:
                            timestamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
                            f.write(f"[{timestamp}] {line}\n")
                            print(f"--> Saved to log file")
                            
                    # Also save full inventory dumps if implemented
                    if "Inventory List:" in line or "Item:" in line:
                         with open(OUTPUT_FILE, 'a', encoding='utf-8') as f:
                            f.write(f"{line}\n")

                except Exception as e:
                    print(f"Error reading line: {e}")
            
            time.sleep(0.01)

    except KeyboardInterrupt:
        print("\nStopping monitor...")
    finally:
        ser.close()

if __name__ == "__main__":
    main()
