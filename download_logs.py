"""
ESP32-C3-LCDKit Clamp Meter & VAMeter Log Downloader Tool
Run this script to download logs over Wi-Fi or USB serial.

Usage:
  python download_logs.py
"""

import os
import sys
import time
import urllib.request

LOGS_DIR = os.path.join(os.path.dirname(__file__), "logs")
os.makedirs(LOGS_DIR, exist_ok=True)

def download_via_wifi():
    print("==================================================")
    print("  ESP32-C3-LCDKit Wi-Fi Log Downloader")
    print("==================================================")
    print("1. Ensure your PC or Phone is connected to Wi-Fi:")
    print("   SSID: ClampMeter-WiFi")
    print("2. Downloading logs from http://192.168.4.1 ...")
    print("--------------------------------------------------")

    url = "http://192.168.4.1/"
    try:
        req = urllib.request.Request(url, headers={'User-Agent': 'Mozilla/5.0'})
        with urllib.request.urlopen(req, timeout=5) as response:
            html = response.read().decode('utf-8')
            
            # Find all download links
            import re
            files = re.findall(r'href=[\'"]/download\?file=([^\'"]+)[\'"]', html)
            if not files:
                print("No CSV log files found on device.")
                return

            print(f"Found {len(files)} log file(s): {', '.join(files)}")
            for filename in files:
                file_url = f"http://192.168.4.1/download?file={filename}"
                target_path = os.path.join(LOGS_DIR, filename)
                print(f"Downloading {filename} -> {target_path} ...", end="", flush=True)
                urllib.request.urlretrieve(file_url, target_path)
                print(" [OK]")

            print("\nAll logs saved successfully to folder:", LOGS_DIR)
    except Exception as e:
        print(f"\nCould not connect to http://192.168.4.1 ({e})")
        print("Make sure the LCDKit is in 'WIFI LOG SERVER' mode and you are connected to 'ClampMeter-WiFi'.")

if __name__ == "__main__":
    download_via_wifi()
