import serial
import serial.tools.list_ports
import time
import os
import sys

def find_feather_port():
    ports = list(serial.tools.list_ports.comports())
    for p in ports:
        if "COM8" in p.device:
            return p.device
    for p in ports:
        if "USB" in p.description or "Serial" in p.description or "JTAG" in p.description:
            return p.device
    return "COM8"

def send_cmd(ser, cmd):
    ser.reset_input_buffer()
    ser.write((cmd + "\n").encode("utf-8"))
    time.sleep(0.15)

def list_files(ser):
    send_cmd(ser, "LIST")
    files = []
    in_list = False
    start = time.time()
    while time.time() - start < 3.0:
        line = ser.readline().decode("utf-8", errors="replace").strip()
        if "<FILE_LIST_START>" in line:
            in_list = True
            continue
        if "<FILE_LIST_END>" in line:
            break
        if in_list and line:
            parts = line.split(",")
            if len(parts) >= 2:
                files.append((parts[0], int(parts[1])))
    return files

def download_file(ser, filename, dest_dir="logs"):
    os.makedirs(dest_dir, exist_ok=True)
    dest_path = os.path.join(dest_dir, filename)
    send_cmd(ser, f"READ {filename}")
    
    in_data = False
    lines = []
    start = time.time()
    while time.time() - start < 10.0:
        raw_line = ser.readline().decode("utf-8", errors="replace")
        if not raw_line:
            continue
        line = raw_line.strip()
        if f"<FILE_DATA_START:{filename}>" in line:
            in_data = True
            continue
        if f"<FILE_DATA_END:{filename}>" in line:
            break
        if in_data:
            lines.append(raw_line)
            start = time.time()
            
    if lines:
        with open(dest_path, "w", encoding="utf-8") as f:
            f.writelines(lines)
        print(f"  [OK] Downloaded {filename} -> {dest_path} ({len(lines)} lines)")
        return True
    else:
        print(f"  [FAIL] Failed to download {filename} (empty or not found)")
        return False

def main():
    port = find_feather_port()
    print("=" * 60)
    print(f"   VAM LOGGER - USB-C SD Card & Telemetry Manager")
    print(f"   Connecting to Feather on {port}...")
    print("=" * 60)

    try:
        ser = serial.Serial(port, 115200, timeout=1.0)
    except Exception as e:
        print(f"Error opening port {port}: {e}")
        return

    time.sleep(0.5)

    while True:
        print("\nChoose an option:")
        print("  1. List CSV logs on SD card")
        print("  2. Download ALL CSV logs to PC (folder: ./logs)")
        print("  3. Download specific CSV log")
        print("  4. View live telemetry status")
        print("  5. Trigger ZERO / TARE calibration")
        print("  6. Reset ZERO / TARE back to raw")
        print("  Q. Quit")
        choice = input("\nEnter choice (1-6, Q): ").strip().upper()

        if choice == "1":
            print("\nReading SD card file list...")
            files = list_files(ser)
            if not files:
                print("No log files found or MicroSD not mounted.")
            else:
                print(f"Found {len(files)} log file(s):")
                for fn, sz in files:
                    print(f"  - {fn:<32} ({sz/1024.0:.1f} KB)")

        elif choice == "2":
            files = list_files(ser)
            if not files:
                print("No log files found on SD card.")
            else:
                print(f"\nDownloading {len(files)} files to ./logs/ ...")
                for fn, sz in files:
                    download_file(ser, fn)
                print("All downloads complete!")

        elif choice == "3":
            fn = input("Enter log filename (e.g. LOG_20261022_131500.csv): ").strip()
            if fn:
                download_file(ser, fn)

        elif choice == "4":
            send_cmd(ser, "STATUS")
            resp = ser.readline().decode("utf-8", errors="replace").strip()
            print(f"\nFeather Telemetry: {resp}")

        elif choice == "5":
            send_cmd(ser, "ZERO")
            resp = ser.readline().decode("utf-8", errors="replace").strip()
            print(f"\nResponse: {resp}")

        elif choice == "6":
            send_cmd(ser, "UNZERO")
            resp = ser.readline().decode("utf-8", errors="replace").strip()
            print(f"\nResponse: {resp}")

        elif choice == "Q":
            print("Exiting.")
            break

    ser.close()

if __name__ == "__main__":
    main()
