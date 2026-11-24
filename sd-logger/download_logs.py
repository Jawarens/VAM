import serial
import serial.tools.list_ports
import time
import os
import sys
import subprocess


def find_feather_port():
    ports = list(serial.tools.list_ports.comports())
    # 1. Prioritize Espressif ESP32-S3 USB Serial/JTAG (VID 0x303A)
    for p in ports:
        hwid = (p.hwid or "").upper()
        if getattr(p, "vid", None) == 0x303A or "303A" in hwid:
            return p.device
    # 2. Fallback to generic USB/Serial/JTAG ports (excluding Intel AMT SOL)
    for p in ports:
        desc = (p.description or "")
        if "Active Management" in desc or "AMT" in desc:
            continue
        if "USB" in desc or "Serial" in desc or "Seriell" in desc or "JTAG" in desc:
            return p.device
    return ports[0].device if ports else "COM8"


def send_cmd(ser, cmd):
    ser.reset_input_buffer()
    ser.write((cmd + "\n").encode("utf-8"))
    ser.flush()
    time.sleep(0.05)


def list_files(ser):
    send_cmd(ser, "LIST")
    files = []
    in_list = False
    start = time.time()
    while time.time() - start < 4.0:
        line = ser.readline().decode("utf-8", errors="replace").strip()
        if "<FILE_LIST_START>" in line:
            in_list = True
            continue
        if "<FILE_LIST_END>" in line:
            break
        if in_list and line:
            parts = line.split(",")
            if len(parts) >= 2:
                try:
                    files.append((parts[0], int(parts[1])))
                except ValueError:
                    pass
    return files


def _format_size(num_bytes):
    if num_bytes >= 1024 * 1024:
        return f"{num_bytes / (1024.0 * 1024.0):.2f} MB"
    return f"{num_bytes / 1024.0:.1f} KB"


def download_file(ser, filename, expected_size=0, dest_dir="logs"):
    os.makedirs(dest_dir, exist_ok=True)
    dest_path = os.path.join(dest_dir, filename)
    send_cmd(ser, f"READ {filename}")

    start_prefix = f"<FILE_DATA_START:{filename}".encode("utf-8")
    end_marker = f"<FILE_DATA_END:{filename}>".encode("utf-8")
    err_prefix = b"<ERROR:"

    buf = bytearray()
    in_data = False
    total_size = expected_size
    bytes_written = 0
    last_activity = time.time()
    t_start = None
    last_print = 0.0
    completed = False

    f = None
    try:
        while time.time() - last_activity < 8.0:
            waiting = ser.in_waiting
            chunk = ser.read(max(16384, waiting) if waiting else 4096)
            if not chunk:
                continue

            last_activity = time.time()
            buf.extend(chunk)

            if not in_data:
                err_idx = buf.find(err_prefix)
                if err_idx != -1:
                    nl = buf.find(b"\n", err_idx)
                    if nl != -1:
                        err_msg = buf[err_idx:nl].decode("utf-8", errors="replace").strip()
                        print(f"\n  [FAIL] {filename}: {err_msg}")
                        return None

                s_idx = buf.find(start_prefix)
                if s_idx != -1:
                    gt_idx = buf.find(b">", s_idx)
                    if gt_idx != -1:
                        nl_idx = buf.find(b"\n", gt_idx)
                        if nl_idx != -1:
                            header_tag = buf[s_idx:gt_idx].decode("utf-8", errors="replace")
                            # Parse optional :<filesize> in <FILE_DATA_START:filename:filesize>
                            tag_parts = header_tag.split(":")
                            if len(tag_parts) >= 3:
                                try:
                                    parsed_sz = int(tag_parts[2])
                                    if parsed_sz > 0:
                                        total_size = parsed_sz
                                except ValueError:
                                    pass
                            del buf[:nl_idx + 1]
                            in_data = True
                            t_start = time.time()
                            f = open(dest_path, "wb")
                        else:
                            continue
                    else:
                        continue
                else:
                    # Avoid unbounded buffer before start marker
                    if len(buf) > 4096:
                        del buf[:-512]
                    continue

            if in_data:
                e_idx = buf.find(end_marker)
                if e_idx != -1:
                    payload = bytes(buf[:e_idx]).replace(b"\r\n", b"\n")
                    if payload.endswith(b"\n"):
                        # Strip the synthetic newline printed right before <FILE_DATA_END:...>
                        payload = payload[:-1]
                    if payload:
                        f.write(payload)
                        bytes_written += len(payload)
                    completed = True
                    break

                # Keep enough tail bytes to catch end_marker across chunk boundaries
                keep_tail = len(end_marker) + 4
                if len(buf) > keep_tail:
                    flush_end = len(buf) - keep_tail
                    # Avoid splitting \r\n across flush boundary
                    if buf[flush_end - 1] == 0x0D:
                        flush_end -= 1
                    if flush_end > 0:
                        payload = bytes(buf[:flush_end]).replace(b"\r\n", b"\n")
                        f.write(payload)
                        bytes_written += len(payload)
                        del buf[:flush_end]

                now = time.time()
                if now - last_print >= 0.15:
                    elapsed = max(0.001, now - t_start)
                    kbps = (bytes_written / 1024.0) / elapsed
                    if total_size > 0:
                        pct = min(100.0, (bytes_written / total_size) * 100.0)
                        print(
                            f"\r  Downloading {filename}: {pct:5.1f}% "
                            f"({_format_size(bytes_written)} / {_format_size(total_size)}) @ {kbps:.0f} KB/s   ",
                            end="",
                            flush=True,
                        )
                    else:
                        print(
                            f"\r  Downloading {filename}: {_format_size(bytes_written)} @ {kbps:.0f} KB/s   ",
                            end="",
                            flush=True,
                        )
                    last_print = now
    finally:
        if f is not None:
            f.close()

    if completed:
        elapsed = max(0.001, time.time() - (t_start or time.time()))
        kbps = (bytes_written / 1024.0) / elapsed
        print(f"\r  [OK] {filename} -> {dest_path} ({_format_size(bytes_written)} in {elapsed:.2f}s, {kbps:.0f} KB/s)          ")
        return dest_path
    else:
        if os.path.exists(dest_path) and bytes_written == 0:
            os.remove(dest_path)
        print(f"\r  [FAIL] Failed to download {filename} (timeout or incomplete)          ")
        return None


def delete_file(ser, filename):
    send_cmd(ser, f"DELETE {filename}")
    start = time.time()
    while time.time() - start < 3.0:
        line = ser.readline().decode("utf-8", errors="replace").strip()
        if line.startswith("OK: DELETED") or line.startswith("ERROR:"):
            print(f"  {line}")
            return line.startswith("OK:")
    print(f"  [TIMEOUT] No response deleting {filename}")
    return False


def delete_all_files(ser):
    send_cmd(ser, "DELETE_ALL")
    start = time.time()
    while time.time() - start < 10.0:
        line = ser.readline().decode("utf-8", errors="replace").strip()
        if line.startswith("OK: DELETED") or line.startswith("ERROR:"):
            print(f"  {line}")
            return line.startswith("OK:")
    print("  [TIMEOUT] No response for DELETE_ALL")
    return False


def toggle_invert(ser, target):
    send_cmd(ser, f"INVERT {target}")
    start = time.time()
    while time.time() - start < 2.0:
        line = ser.readline().decode("utf-8", errors="replace").strip()
        if line.startswith("OK: INVERT"):
            print(f"  {line}")
            return
    print("  [TIMEOUT] No response from device.")


def run_i2c_scan(ser):
    send_cmd(ser, "I2C_SCAN")
    start = time.time()
    while time.time() - start < 4.0:
        line = ser.readline().decode("utf-8", errors="replace").strip()
        if line:
            print(f"  {line}")
            if "I2C scan complete" in line or line.startswith("ERROR:"):
                break


def parse_selection(sel_str, files):
    sel_str = sel_str.strip()
    if not sel_str:
        return []
    # Match exact filename first
    for fn, sz in files:
        if fn == sel_str:
            return [(fn, sz)]
    selected = []
    seen = set()
    for part in sel_str.split(","):
        part = part.strip()
        if "-" in part:
            bounds = part.split("-", 1)
            if bounds[0].isdigit() and bounds[1].isdigit():
                a, b = int(bounds[0]), int(bounds[1])
                for idx in range(min(a, b), max(a, b) + 1):
                    if 1 <= idx <= len(files) and idx not in seen:
                        selected.append(files[idx - 1])
                        seen.add(idx)
        elif part.isdigit():
            idx = int(part)
            if 1 <= idx <= len(files) and idx not in seen:
                selected.append(files[idx - 1])
                seen.add(idx)
    return selected


def print_file_list(files):
    if not files:
        print("No log files found or MicroSD not mounted.")
        return
    total_sz = sum(sz for _, sz in files)
    print(f"Found {len(files)} log file(s) ({_format_size(total_sz)} total):")
    for idx, (fn, sz) in enumerate(files, 1):
        print(f"  [{idx:>2}] {fn:<32} ({_format_size(sz)})")


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
        try:
            ser.set_buffer_size(rx_size=262144, tx_size=16384)
        except Exception:
            pass
    except Exception as e:
        print(f"Error opening port {port}: {e}")
        print("Note: You can still view previously downloaded logs using log_viewer.py!")
        choice = input("\nWould you like to launch the Log Viewer anyway? (Y/N): ").strip().upper()
        if choice == "Y":
            launch_viewer()
        return

    time.sleep(0.2)

    while True:
        print("\n" + "-" * 56)
        print("Choose an option:")
        print("   1. List CSV logs on SD card")
        print("   2. Download ALL CSV logs to PC (./logs)")
        print("   3. Download specific CSV log(s)")
        print("   4. Delete specific CSV log(s) from SD card")
        print("   5. Delete ALL CSV logs from SD card")
        print("   6. View live telemetry snapshot (STATUS) + I2C scan")
        print("   7. Trigger ZERO / TARE calibration (or Reset)")
        print("   8. Toggle LEM polarity inversion (DHAB / HO-100-S)")
        print("   9. Open Graph Viewer (Desktop GUI)")
        print("  10. Export Newest Log to Excel (.xlsx with charts)")
        print("  11. Generate Interactive HTML Report")
        print("   Q. Quit")
        choice = input("\nEnter choice (1-11, Q): ").strip().upper()

        if choice == "1":
            print("\nReading SD card file list...")
            files = list_files(ser)
            print_file_list(files)

        elif choice == "2":
            files = list_files(ser)
            if not files:
                print("No log files found on SD card.")
            else:
                print_file_list(files)
                print(f"\nDownloading {len(files)} file(s) to ./logs/ ...")
                downloaded_names = []
                last_downloaded = None
                for fn, sz in files:
                    p = download_file(ser, fn, expected_size=sz)
                    if p:
                        downloaded_names.append(fn)
                        last_downloaded = p
                print(f"Downloads complete ({len(downloaded_names)}/{len(files)} succeeded).")
                if downloaded_names:
                    del_c = input("Delete successfully downloaded files from SD card? (Y/N): ").strip().upper()
                    if del_c == "Y":
                        for fn in downloaded_names:
                            delete_file(ser, fn)
                if last_downloaded:
                    c = input("Open newest in Graph Viewer now? (Y/N): ").strip().upper()
                    if c == "Y":
                        launch_viewer(last_downloaded)

        elif choice == "3":
            files = list_files(ser)
            if not files:
                print("No log files found on SD card.")
            else:
                print_file_list(files)
                sel = input("\nEnter file number(s) (e.g. 1, 1-3) or filename: ").strip()
                targets = parse_selection(sel, files)
                if not targets:
                    print("No matching files selected.")
                else:
                    downloaded_names = []
                    last_downloaded = None
                    for fn, sz in targets:
                        p = download_file(ser, fn, expected_size=sz)
                        if p:
                            downloaded_names.append(fn)
                            last_downloaded = p
                    if downloaded_names:
                        del_c = input("Delete downloaded file(s) from SD card? (Y/N): ").strip().upper()
                        if del_c == "Y":
                            for fn in downloaded_names:
                                delete_file(ser, fn)
                    if last_downloaded:
                        c = input("Open in Graph Viewer now? (Y/N): ").strip().upper()
                        if c == "Y":
                            launch_viewer(last_downloaded)

        elif choice == "4":
            files = list_files(ser)
            if not files:
                print("No log files found on SD card.")
            else:
                print_file_list(files)
                sel = input("\nEnter file number(s) to DELETE (e.g. 1, 1-3) or filename: ").strip()
                targets = parse_selection(sel, files)
                if not targets:
                    print("No matching files selected.")
                else:
                    confirm = input(f"Confirm permanent deletion of {len(targets)} file(s)? (Y/N): ").strip().upper()
                    if confirm == "Y":
                        for fn, _ in targets:
                            delete_file(ser, fn)

        elif choice == "5":
            confirm = input("Type YES to permanently DELETE ALL CSV logs on the SD card: ").strip()
            if confirm == "YES":
                delete_all_files(ser)
            else:
                print("Cancelled.")

        elif choice == "6":
            send_cmd(ser, "STATUS")
            resp = ser.readline().decode("utf-8", errors="replace").strip()
            print(f"\nFeather Telemetry:\n  {resp}")
            print("\nI2C Bus Scan:")
            run_i2c_scan(ser)

        elif choice == "7":
            sub = input("  [Z]ero/Tare now, or [R]eset back to raw? (Z/R): ").strip().upper()
            if sub == "Z":
                send_cmd(ser, "ZERO")
                print(f"  Response: {ser.readline().decode('utf-8', errors='replace').strip()}")
            elif sub == "R":
                send_cmd(ser, "UNZERO")
                print(f"  Response: {ser.readline().decode('utf-8', errors='replace').strip()}")

        elif choice == "8":
            toggle_invert(ser, "STATUS")
            sub = input("  Toggle [D]HAB, [H]O-100-S, [B]oth, or [R]eset to normal? (D/H/B/R): ").strip().upper()
            mapping = {"D": "DHAB", "H": "HO100", "B": "BOTH", "R": "RESET"}
            if sub in mapping:
                toggle_invert(ser, mapping[sub])

        elif choice == "9":
            print("\nLaunching Log Viewer GUI...")
            launch_viewer()

        elif choice == "10":
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

        elif choice == "11":
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
