import serial
import sys
import time

port = sys.argv[1] if len(sys.argv) > 1 else "COM4"
baud = int(sys.argv[2]) if len(sys.argv) > 2 else 115200

try:
    ser = serial.Serial(port, baud, timeout=0.1)
    # Proper reset: RTS=True (EN=LOW), DTR=False (IO0=HIGH)
    ser.setDTR(False)
    ser.setRTS(True)
    time.sleep(0.1)
    ser.setRTS(False)
    time.sleep(0.1)
    
    start = time.time()
    while time.time() - start < 4.0:
        raw = ser.read(1024)
        if raw:
            sys.stdout.write(raw.decode('utf-8', errors='replace'))
            sys.stdout.flush()
    ser.close()
except Exception as e:
    print(f"Error: {e}")
