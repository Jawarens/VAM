import serial
import serial.tools.list_ports
import time
import os
import sys
import subprocess

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
        return dest_path
    else:
        print(f"  [FAIL] Failed to download {filename} (empty or not found)")
        return None

def launch_viewer(target_file=None):
    cmd = [sys.executable, "log_viewer.py"]
    if target_file:
        cmd.append(target_file)
    subprocess.Popen(cmd)

def main():
    port = find_feather_port()
    print("=" * 65)
    print("      VAM LOGGER - USB-C Telemetry & Log Analysis Suite")
    print(f"      Connecting to Feather on {port}...")
    print("=" * 65)

    try:
        ser = serial.Serial()
        ser.port = port
        ser.baudrate = 115200
        ser.timeout = 1.0
        ser.dtr = False
        ser.rts = False
        ser.open()
    except Exception as e:
        print(f"Error opening port {port}: {e}")
        print("Note: You can still view previously downloaded logs using log_viewer.py!")
        choice = input("\nWould you like to launch the Log Viewer anyway? (Y/N): ").strip().upper()
        if choice == "Y":
            launch_viewer()
        return

    time.sleep(0.3)

    while True:
        print("\n" + "-" * 50)
        print("Choose an option:")
        print("  1. List CSV logs on SD card")
        print("  2. Download ALL CSV logs to PC (folder: ./logs)")
        print("  3. Download specific CSV log")
        print("  4. View live telemetry snapshot (Status)")
        print("  5. Trigger ZERO / TARE calibration")
        print("  6. Reset ZERO / TARE back to raw")
        print("  7. 📈 Open Graph Viewer (Desktop GUI)")
        print("  8. 📊 Export Newest Log to Excel (.xlsx with charts)")
        print("  9. 🌐 Generate Interactive HTML Report")
        print("  Q. Quit")
        choice = input("\nEnter choice (1-9, Q): ").strip().upper()

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
                last_downloaded = None
                for fn, sz in files:
                    p = download_file(ser, fn)
                    if p: last_downloaded = p
                print("All downloads complete!")
                if last_downloaded:
                    c = input("Open in Graph Viewer now? (Y/N): ").strip().upper()
                    if c == "Y":
                        launch_viewer(last_downloaded)

        elif choice == "3":
            fn = input("Enter log filename (e.g. LOG_20261023_123839.csv): ").strip()
            if fn:
                p = download_file(ser, fn)
                if p:
                    c = input("Open in Graph Viewer now? (Y/N): ").strip().upper()
                    if c == "Y":
                        launch_viewer(p)

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

        elif choice == "7":
            print("\nLaunching Log Viewer GUI...")
            launch_viewer()

        elif choice == "8":
            logs_dir = "logs"
            if os.path.exists(logs_dir):
                csvs = [os.path.join(logs_dir, f) for f in os.listdir(logs_dir) if f.endswith(".csv")]
                if csvs:
                    newest = max(csvs, key=os.path.getmtime)
                    print(f"Exporting {newest} to Excel...")
                    subprocess.run([sys.executable, "log_viewer.py", newest, "--excel"])
                else:
                    print("No CSV files found in ./logs. Please download one first!")
            else:
                print("No ./logs directory found.")

        elif choice == "9":
            logs_dir = "logs"
            if os.path.exists(logs_dir):
                csvs = [os.path.join(logs_dir, f) for f in os.listdir(logs_dir) if f.endswith(".csv")]
                if csvs:
                    newest = max(csvs, key=os.path.getmtime)
                    print(f"Generating HTML report for {newest}...")
                    subprocess.run([sys.executable, "log_viewer.py", newest, "--html"])
                    html_file = os.path.splitext(newest)[0] + "_report.html"
                    if os.path.exists(html_file):
                        import webbrowser
                        webbrowser.open(os.path.abspath(html_file))
                else:
                    print("No CSV files found in ./logs. Please download one first!")
            else:
                print("No ./logs directory found.")

        elif choice == "Q":
            print("Exiting.")
            break

    ser.close()

if __name__ == "__main__":
    main()
