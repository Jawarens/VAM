"""
VAM Multi-Channel Telemetry Logger - Log Viewer & Analysis Suite
Supports:
  - Multi-channel currents (INA1, INA2, DHAB Low/High ranges, HO-100-S, Total)
  - Interactive GUI with Matplotlib graphing & channel toggling
  - Excel (.xlsx) export with native embedded charts and Ah/Wh integration
  - Interactive HTML report (zoomable, pan, hover tooltips via Chart.js)
  - Statistical summary (Peak, RMS, Avg, Min, Ah, Wh) per module
"""

import os
import sys
import argparse
import datetime
import io
import re
import pandas as pd
import numpy as np

# Module Descriptions for clear user understanding
CHANNEL_INFO = {
    "INA1": {
        "name": "INA228 Shunt #1 (0x40)",
        "desc": "Primary high-precision bidirectional current shunt & bus voltage",
        "current_col": "INA1_A",
        "voltage_col": "INA1_V",
        "power_col": "INA1_W",
        "color": "#1f77b4"  # Blue
    },
    "INA2": {
        "name": "INA228 Shunt #2 (0x41)",
        "desc": "Secondary high-precision bidirectional current shunt & bus voltage",
        "current_col": "INA2_A",
        "voltage_col": "INA2_V",
        "power_col": "INA2_W",
        "color": "#ff7f0e"  # Orange
    },
    "DHAB_LOW": {
        "name": "LEM DHAB S/157 (Low Range)",
        "desc": "High sensitivity channel (40 mV/A, +/-50A nominal) for high precision",
        "current_col": "DHAB_Low_A",
        "color": "#22c55e"  # Bright Green
    },
    "DHAB_HIGH": {
        "name": "LEM DHAB S/157 (High Range)",
        "desc": "High current channel (4 mV/A, +/-500A nominal) for heavy load / cranking",
        "current_col": "DHAB_High_A",
        "color": "#15803d"  # Forest Green
    },
    "DHAB": {
        "name": "LEM DHAB S/157 (Auto-Selected)",
        "desc": "Firmware auto-switched between Low (<42A) and High (>42A) ranges",
        "current_col": "DHAB_A",
        "color": "#2ca02c"
    },
    "HO100": {
        "name": "LEM HO-100-S Hall Clamp",
        "desc": "High-speed open-loop Hall effect clamp (+/-100A peak)",
        "current_col": "HO100_A",
        "color": "#d62728"  # Red
    },
    "TOTAL": {
        "name": "Total System Current",
        "desc": "Aggregate calculated system load current",
        "current_col": "Total_Current_A",
        "color": "#9467bd"  # Purple
    },
    "POWER": {
        "name": "Total System Power",
        "desc": "Real-time system electrical power (Watts)",
        "power_col": "Total_Power_W",
        "color": "#8c564b"  # Brown
    },
    "BATT": {
        "name": "18650 Battery Pack",
        "desc": "Logger internal power status (MAX17048 Fuel Gauge)",
        "voltage_col": "Batt_V",
        "pct_col": "Batt_Pct",
        "color": "#17becf"  # Cyan
    }
}

def _trapz(y, x):
    """Compat wrapper for NumPy 2.x (trapezoid) and 1.x (trapz)."""
    if hasattr(np, "trapezoid"):
        return np.trapezoid(y, x)
    return np.trapz(y, x)


def _detect_csv_format(text):
    """Auto-detects column separator (';' or ',') and decimal char ('.' or ',')."""
    lines = [ln for ln in text.splitlines() if ln.strip()]
    if not lines:
        return ",", ".", text

    sep = None
    if lines[0].lower().startswith("sep=") and len(lines[0]) >= 5:
        sep = lines[0][4]
        text = "\n".join(lines[1:])
        lines = lines[1:]

    if not lines:
        return (sep or ","), ".", text

    header = lines[0]
    if sep is None:
        sep = ";" if header.count(";") >= header.count(",") else ","

    decimal = "."
    if sep == ";":
        # Check numeric columns (after col 0 Timestamp_ISO) for decimal commas
        comma_hits = 0
        dot_hits = 0
        for sample_line in lines[1:6]:
            parts = sample_line.split(";")
            for val in parts[1:]:
                if "," in val:
                    comma_hits += 1
                if "." in val:
                    dot_hits += 1
        if comma_hits > dot_hits:
            decimal = ","

    return sep, decimal, text


def load_log_file(filepath):
    """Loads and sanitizes CSV log file into a pandas DataFrame."""
    if not os.path.exists(filepath):
        raise FileNotFoundError(f"File not found: {filepath}")

    with open(filepath, "r", encoding="utf-8-sig", errors="replace") as f:
        content = f.read()

    sep, decimal, clean_content = _detect_csv_format(content)

    # Repair rows where a newline was dropped between timestamps (e.g. from UART buffer overrun)
    fixed_content = re.sub(r'([^\r\n])(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2})', r'\1\n\2', clean_content)

    try:
        df = pd.read_csv(
            io.StringIO(fixed_content),
            sep=sep,
            decimal=decimal,
            on_bad_lines="skip",
            low_memory=False,
        )
    except TypeError:
        df = pd.read_csv(
            io.StringIO(fixed_content),
            sep=sep,
            decimal=decimal,
            error_bad_lines=False,
            warn_bad_lines=False,
            low_memory=False,
        )

    df.columns = [c.strip() for c in df.columns]

    numeric_cols = [
        "Unix_ms", "Sample_Idx",
        "INA1_V", "INA1_A", "INA1_W",
        "INA2_V", "INA2_A", "INA2_W",
        "DHAB_Low_A", "DHAB_High_A", "DHAB_A",
        "HO100_A", "Total_Current_A", "Total_Power_W",
        "Batt_V", "Batt_Pct",
    ]
    for col in numeric_cols:
        if col in df.columns:
            df[col] = pd.to_numeric(df[col], errors="coerce")

    before_len = len(df)
    valid_mask = pd.Series(True, index=df.index)
    if "Sample_Idx" in df.columns:
        valid_mask &= df["Sample_Idx"].notna()

    # Convert timestamp
    if "Timestamp_ISO" in df.columns:
        df["DateTime"] = pd.to_datetime(df["Timestamp_ISO"], errors="coerce")
        if before_len > 0 and not df["DateTime"].isna().all():
            valid_mask &= df["DateTime"].notna()

    present_val_cols = [c for c in ["INA1_A", "INA2_A", "DHAB_A", "HO100_A", "Total_Current_A", "Batt_V"] if c in df.columns]
    if present_val_cols and before_len > 0:
        valid_mask &= df[present_val_cols].notna().any(axis=1)

    df = df[valid_mask].reset_index(drop=True)
    skipped_rows = before_len - len(df)

    if len(df) == 0:
        df["Time_Sec"] = pd.Series(dtype=float)
    elif "DateTime" in df.columns and not df["DateTime"].isna().all():
        t0 = df["DateTime"].iloc[0]
        df["Time_Sec"] = (df["DateTime"] - t0).dt.total_seconds()
    elif "Unix_ms" in df.columns and not df["Unix_ms"].isna().all():
        t0 = df["Unix_ms"].iloc[0]
        df["Time_Sec"] = (df["Unix_ms"] - t0) / 1000.0
    else:
        df["Time_Sec"] = df.index * 0.5

    df.attrs["skipped_rows"] = skipped_rows
    return df


def apply_polarity_inversion(df, invert_dhab=False, invert_ho100=False):
    """Returns a copy of df with LEM DHAB and/or HO-100-S polarities inverted and totals recomputed."""
    if df is None:
        return None
    out = df.copy()
    if not invert_dhab and not invert_ho100:
        out.attrs = dict(df.attrs)
        return out

    if invert_dhab:
        for col in ("DHAB_Low_A", "DHAB_High_A", "DHAB_A"):
            if col in out.columns:
                out[col] = -out[col]

    if invert_ho100 and "HO100_A" in out.columns:
        out["HO100_A"] = -out["HO100_A"]

    zeros = pd.Series(0.0, index=out.index)
    ina1_a = out["INA1_A"].fillna(0.0) if "INA1_A" in out.columns else zeros
    ina2_a = out["INA2_A"].fillna(0.0) if "INA2_A" in out.columns else zeros
    dhab_a = out["DHAB_A"].fillna(0.0) if "DHAB_A" in out.columns else zeros
    ho100_a = out["HO100_A"].fillna(0.0) if "HO100_A" in out.columns else zeros

    if "Total_Current_A" in out.columns:
        out["Total_Current_A"] = ina1_a + ina2_a + dhab_a + ho100_a

    if "Total_Power_W" in out.columns:
        ina1_w = out["INA1_W"].fillna(0.0) if "INA1_W" in out.columns else zeros
        ina2_w = out["INA2_W"].fillna(0.0) if "INA2_W" in out.columns else zeros
        ina1_v = out["INA1_V"].fillna(0.0) if "INA1_V" in out.columns else zeros
        ina2_v = out["INA2_V"].fillna(0.0) if "INA2_V" in out.columns else zeros
        v_primary = np.where(ina1_v > 0.5, ina1_v, np.where(ina2_v > 0.5, ina2_v, 12.0))
        out["Total_Power_W"] = ina1_w + ina2_w + v_primary * (dhab_a + ho100_a)

    out.attrs = dict(df.attrs)
    return out


def compute_statistics(df):
    """Computes comprehensive statistics and energy metrics (Ah, Wh)."""
    stats = {}
    if len(df) > 1:
        time_span_s = float(df["Time_Sec"].max() - df["Time_Sec"].min())
    else:
        time_span_s = 0.0

    stats["Duration_Sec"] = time_span_s
    stats["Duration_Str"] = str(datetime.timedelta(seconds=int(max(0.0, time_span_s))))
    stats["Total_Samples"] = len(df)
    stats["Sample_Rate_Hz"] = (len(df) / time_span_s) if time_span_s > 0 else 0.0

    current_cols = ["INA1_A", "INA2_A", "DHAB_Low_A", "DHAB_High_A", "DHAB_A", "HO100_A", "Total_Current_A"]
    stats["Channels"] = {}

    for col in current_cols:
        if col in df.columns and len(df) > 0:
            s = pd.to_numeric(df[col], errors="coerce").fillna(0.0)
            ah = float(_trapz(s, df["Time_Sec"]) / 3600.0) if len(df) > 1 else 0.0
            stats["Channels"][col] = {
                "min": float(s.min()),
                "max": float(s.max()),
                "mean": float(s.mean()),
                "std": float(s.std()) if len(df) > 1 else 0.0,
                "ah": ah
            }

    if "Total_Power_W" in df.columns and len(df) > 0:
        p = pd.to_numeric(df["Total_Power_W"], errors="coerce").fillna(0.0)
        wh = float(_trapz(p, df["Time_Sec"]) / 3600.0) if len(df) > 1 else 0.0
        stats["Power"] = {
            "min": float(p.min()),
            "max": float(p.max()),
            "mean": float(p.mean()),
            "wh": wh
        }

    return stats

def export_html_report(df, csv_path, output_html=None):
    """Generates an interactive, standalone HTML dashboard with Chart.js."""
    if output_html is None:
        base = os.path.splitext(csv_path)[0]
        output_html = f"{base}_report.html"

    stats = compute_statistics(df)
    filename = os.path.basename(csv_path)

    # Downsample if log is very large (>4000 points) for fast browser rendering
    max_pts = 4000
    if len(df) > max_pts:
        step = int(np.ceil(len(df) / max_pts))
        plot_df = df.iloc[::step].copy()
    else:
        plot_df = df.copy()

    times = [f"{t:.2f}" for t in plot_df["Time_Sec"].tolist()]
    
    ina1_a = plot_df["INA1_A"].round(4).tolist() if "INA1_A" in plot_df else []
    ina2_a = plot_df["INA2_A"].round(4).tolist() if "INA2_A" in plot_df else []
    dhab_low = plot_df["DHAB_Low_A"].round(4).tolist() if "DHAB_Low_A" in plot_df else []
    dhab_high = plot_df["DHAB_High_A"].round(4).tolist() if "DHAB_High_A" in plot_df else []
    dhab_comb = plot_df["DHAB_A"].round(4).tolist() if "DHAB_A" in plot_df else []
    ho100_a = plot_df["HO100_A"].round(4).tolist() if "HO100_A" in plot_df else []
    tot_a = plot_df["Total_Current_A"].round(4).tolist() if "Total_Current_A" in plot_df else []

    ina1_v = plot_df["INA1_V"].round(3).tolist() if "INA1_V" in plot_df else []
    ina2_v = plot_df["INA2_V"].round(3).tolist() if "INA2_V" in plot_df else []
    batt_v = plot_df["Batt_V"].round(2).tolist() if "Batt_V" in plot_df else []

    pwr_w = plot_df["Total_Power_W"].round(2).tolist() if "Total_Power_W" in plot_df else []

    html_content = f"""<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>VAM Logger Telemetry Report - {filename}</title>
    <script src="https://cdn.jsdelivr.net/npm/chart.js"></script>
    <style>
        :root {{
            --bg-color: #0f172a;
            --card-bg: #1e293b;
            --text-main: #f8fafc;
            --text-dim: #94a3b8;
            --border-color: #334155;
            --accent-cyan: #38bdf8;
            --accent-green: #4ade80;
            --accent-yellow: #facc15;
            --accent-red: #f87171;
            --accent-purple: #c084fc;
        }}
        body {{
            font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Helvetica, Arial, sans-serif;
            background-color: var(--bg-color);
            color: var(--text-main);
            margin: 0;
            padding: 24px;
        }}
        .header {{
            display: flex;
            justify-content: space-between;
            align-items: center;
            border-bottom: 2px solid var(--border-color);
            padding-bottom: 16px;
            margin-bottom: 24px;
        }}
        .title h1 {{
            margin: 0;
            font-size: 26px;
            color: var(--accent-cyan);
            letter-spacing: 0.5px;
        }}
        .title p {{
            margin: 4px 0 0 0;
            color: var(--text-dim);
            font-size: 14px;
        }}
        .badge {{
            background: #0369a1;
            color: white;
            padding: 6px 14px;
            border-radius: 9999px;
            font-weight: 600;
            font-size: 13px;
        }}
        .kpi-grid {{
            display: grid;
            grid-template-columns: repeat(auto-fit, minmax(200px, 1fr));
            gap: 16px;
            margin-bottom: 24px;
        }}
        .kpi-card {{
            background: var(--card-bg);
            border: 1px solid var(--border-color);
            border-radius: 12px;
            padding: 16px;
            box-shadow: 0 4px 6px -1px rgba(0,0,0,0.2);
        }}
        .kpi-card .label {{
            font-size: 12px;
            color: var(--text-dim);
            text-transform: uppercase;
            font-weight: bold;
            margin-bottom: 6px;
        }}
        .kpi-card .value {{
            font-size: 24px;
            font-weight: bold;
            color: var(--text-main);
        }}
        .kpi-card .sub {{
            font-size: 12px;
            color: var(--text-dim);
            margin-top: 4px;
        }}
        .chart-box {{
            background: var(--card-bg);
            border: 1px solid var(--border-color);
            border-radius: 12px;
            padding: 20px;
            margin-bottom: 24px;
        }}
        .chart-header {{
            display: flex;
            justify-content: space-between;
            align-items: center;
            margin-bottom: 16px;
        }}
        .chart-header h2 {{
            margin: 0;
            font-size: 18px;
            color: var(--accent-cyan);
        }}
        .legend-guide {{
            background: rgba(15, 23, 42, 0.6);
            border: 1px solid var(--border-color);
            border-radius: 8px;
            padding: 12px 16px;
            margin-bottom: 20px;
            display: flex;
            flex-wrap: wrap;
            gap: 18px;
            font-size: 13px;
        }}
        .module-item {{
            display: flex;
            align-items: center;
            gap: 8px;
        }}
        .dot {{
            width: 12px;
            height: 12px;
            border-radius: 50%;
            display: inline-block;
        }}
        canvas {{
            max-height: 420px;
            width: 100% !important;
        }}
    </style>
</head>
<body>

    <div class="header">
        <div class="title">
            <h1>⚡ VAM Multi-Channel Telemetry Viewer</h1>
            <p>Log File: <strong>{filename}</strong> | Recorded via Adafruit Feather ESP32-S3</p>
        </div>
        <div class="badge">10 Hz Telemetry Log</div>
    </div>

    <!-- KPI Summary Grid -->
    <div class="kpi-grid">
        <div class="kpi-card">
            <div class="label">Duration</div>
            <div class="value">{stats['Duration_Str']}</div>
            <div class="sub">{stats['Total_Samples']} samples @ {stats['Sample_Rate_Hz']:.1f} Hz</div>
        </div>
        <div class="kpi-card">
            <div class="label">Max System Current</div>
            <div class="value" style="color: var(--accent-yellow);">{stats['Channels'].get('Total_Current_A', {}).get('max', 0):.2f} A</div>
            <div class="sub">Min: {stats['Channels'].get('Total_Current_A', {}).get('min', 0):.2f} A</div>
        </div>
        <div class="kpi-card">
            <div class="label">Total Energy (Ah)</div>
            <div class="value" style="color: var(--accent-green);">{stats['Channels'].get('Total_Current_A', {}).get('ah', 0):.4f} Ah</div>
            <div class="sub">Integrated System Load</div>
        </div>
        <div class="kpi-card">
            <div class="label">Total Energy (Wh)</div>
            <div class="value" style="color: var(--accent-purple);">{stats.get('Power', {}).get('wh', 0):.2f} Wh</div>
            <div class="sub">Peak Power: {stats.get('Power', {}).get('max', 0):.1f} W</div>
        </div>
        <div class="kpi-card">
            <div class="label">Logger Battery</div>
            <div class="value" style="color: var(--accent-cyan);">{plot_df['Batt_V'].iloc[-1] if ('Batt_V' in plot_df and len(plot_df) > 0) else 0:.2f} V</div>
            <div class="sub">18650 Pack: {int(plot_df['Batt_Pct'].iloc[-1]) if ('Batt_Pct' in plot_df and len(plot_df) > 0) else 0}% remaining</div>
        </div>
    </div>

    <!-- Module Guide -->
    <div class="legend-guide">
        <div class="module-item">
            <span class="dot" style="background: #38bdf8;"></span>
            <strong>INA1 (0x40):</strong> Primary Precision Shunt
        </div>
        <div class="module-item">
            <span class="dot" style="background: #fb923c;"></span>
            <strong>INA2 (0x41):</strong> Secondary Shunt
        </div>
        <div class="module-item">
            <span class="dot" style="background: #4ade80;"></span>
            <strong>DHAB Low (±50A):</strong> High Sensitivity Channel (40mV/A)
        </div>
        <div class="module-item">
            <span class="dot" style="background: #15803d;"></span>
            <strong>DHAB High (±500A):</strong> High Current Channel (4mV/A)
        </div>
        <div class="module-item">
            <span class="dot" style="background: #f87171;"></span>
            <strong>HO-100-S:</strong> Fast Hall Clamp (±100A)
        </div>
        <div class="module-item">
            <span class="dot" style="background: #c084fc;"></span>
            <strong>Total Load:</strong> Combined System Current
        </div>
    </div>

    <!-- Current Chart -->
    <div class="chart-box">
        <div class="chart-header">
            <h2>📈 Multi-Channel Currents (Amperes)</h2>
            <small style="color: var(--text-dim);">Click any legend item to toggle channels ON/OFF</small>
        </div>
        <canvas id="currentChart"></canvas>
    </div>

    <!-- Voltage Chart -->
    <div class="chart-box">
        <div class="chart-header">
            <h2>⚡ Bus Voltages & Logger Battery (Volts)</h2>
            <small style="color: var(--text-dim);">INA1 / INA2 High-Voltage Inputs + 18650 Supply</small>
        </div>
        <canvas id="voltageChart"></canvas>
    </div>

    <!-- Power Chart -->
    <div class="chart-box">
        <div class="chart-header">
            <h2>🔥 Total Power Consumption (Watts)</h2>
            <small style="color: var(--text-dim);">Aggregated Dynamic Electrical Power</small>
        </div>
        <canvas id="powerChart"></canvas>
    </div>

    <script>
        const times = {times};

        // 1. Current Chart
        new Chart(document.getElementById('currentChart'), {{
            type: 'line',
            data: {{
                labels: times,
                datasets: [
                    {{ label: 'Total Current (A)', data: {tot_a}, borderColor: '#c084fc', backgroundColor: 'rgba(192, 132, 252, 0.1)', borderWidth: 2.5, pointRadius: 0 }},
                    {{ label: 'INA1 Shunt (A)', data: {ina1_a}, borderColor: '#38bdf8', borderWidth: 1.5, pointRadius: 0 }},
                    {{ label: 'INA2 Shunt (A)', data: {ina2_a}, borderColor: '#fb923c', borderWidth: 1.5, pointRadius: 0 }},
                    {{ label: 'DHAB Low (±50A)', data: {dhab_low if dhab_low else dhab_comb}, borderColor: '#4ade80', borderWidth: 1.5, pointRadius: 0 }},
                    {{ label: 'DHAB High (±500A)', data: {dhab_high}, borderColor: '#15803d', borderWidth: 1.5, borderDash: [4, 4], pointRadius: 0 }},
                    {{ label: 'HO100 Clamp (A)', data: {ho100_a}, borderColor: '#f87171', borderWidth: 1.5, pointRadius: 0 }}
                ]
            }},
            options: {{
                responsive: true,
                interaction: {{ mode: 'index', intersect: false }},
                scales: {{
                    x: {{ title: {{ display: true, text: 'Time Elapsed (Seconds)', color: '#94a3b8' }}, grid: {{ color: '#334155' }}, ticks: {{ color: '#94a3b8' }} }},
                    y: {{ title: {{ display: true, text: 'Current (A)', color: '#94a3b8' }}, grid: {{ color: '#334155' }}, ticks: {{ color: '#94a3b8' }} }}
                }},
                plugins: {{
                    legend: {{ labels: {{ color: '#f8fafc', font: {{ size: 13 }} }} }}
                }}
            }}
        }});

        // 2. Voltage Chart
        new Chart(document.getElementById('voltageChart'), {{
            type: 'line',
            data: {{
                labels: times,
                datasets: [
                    {{ label: 'INA1 Bus Voltage (V)', data: {ina1_v}, borderColor: '#38bdf8', borderWidth: 2, pointRadius: 0 }},
                    {{ label: 'INA2 Bus Voltage (V)', data: {ina2_v}, borderColor: '#fb923c', borderWidth: 2, pointRadius: 0 }},
                    {{ label: 'Feather 18650 Battery (V)', data: {batt_v}, borderColor: '#facc15', borderWidth: 1.8, borderDash: [5, 5], pointRadius: 0 }}
                ]
            }},
            options: {{
                responsive: true,
                interaction: {{ mode: 'index', intersect: false }},
                scales: {{
                    x: {{ title: {{ display: true, text: 'Time Elapsed (Seconds)', color: '#94a3b8' }}, grid: {{ color: '#334155' }}, ticks: {{ color: '#94a3b8' }} }},
                    y: {{ title: {{ display: true, text: 'Voltage (V)', color: '#94a3b8' }}, grid: {{ color: '#334155' }}, ticks: {{ color: '#94a3b8' }} }}
                }},
                plugins: {{
                    legend: {{ labels: {{ color: '#f8fafc', font: {{ size: 13 }} }} }}
                }}
            }}
        }});

        // 3. Power Chart
        new Chart(document.getElementById('powerChart'), {{
            type: 'line',
            data: {{
                labels: times,
                datasets: [
                    {{ label: 'Total Power (W)', data: {pwr_w}, borderColor: '#f43f5e', backgroundColor: 'rgba(244, 63, 94, 0.15)', fill: true, borderWidth: 2, pointRadius: 0 }}
                ]
            }},
            options: {{
                responsive: true,
                interaction: {{ mode: 'index', intersect: false }},
                scales: {{
                    x: {{ title: {{ display: true, text: 'Time Elapsed (Seconds)', color: '#94a3b8' }}, grid: {{ color: '#334155' }}, ticks: {{ color: '#94a3b8' }} }},
                    y: {{ title: {{ display: true, text: 'Power (Watts)', color: '#94a3b8' }}, grid: {{ color: '#334155' }}, ticks: {{ color: '#94a3b8' }} }}
                }},
                plugins: {{
                    legend: {{ labels: {{ color: '#f8fafc', font: {{ size: 13 }} }} }}
                }}
            }}
        }});
    </script>
</body>
</html>
"""
    with open(output_html, "w", encoding="utf-8") as f:
        f.write(html_content)

    print(f"  [OK] Exported Interactive HTML Report -> {output_html}")
    return output_html

def export_excel_report(df, csv_path, output_xlsx=None):
    """Exports structured Excel workbook with native Excel charts using openpyxl."""
    import openpyxl
    from openpyxl.chart import LineChart, Reference
    from openpyxl.styles import Font, PatternFill, Alignment, Border, Side
    from openpyxl.utils.dataframe import dataframe_to_rows

    if output_xlsx is None:
        base = os.path.splitext(csv_path)[0]
        output_xlsx = f"{base}_analyzed.xlsx"

    wb = openpyxl.Workbook()

    # 1. Sheet 1: Summary & Module Guide
    ws_summary = wb.active
    ws_summary.title = "Summary & Module Guide"
    ws_summary.views.sheetView[0].showGridLines = True

    header_font = Font(name="Segoe UI", size=14, bold=True, color="1E3A8A")
    title_cell = ws_summary.cell(row=1, column=1, value="VAM Multi-Channel Telemetry Summary")
    title_cell.font = header_font

    ws_summary.cell(row=2, column=1, value=f"Log File: {os.path.basename(csv_path)}")
    ws_summary.cell(row=3, column=1, value=f"Export Date: {datetime.datetime.now().strftime('%Y-%m-%d %H:%M:%S')}")

    stats = compute_statistics(df)
    ws_summary.cell(row=5, column=1, value="Session Metrics").font = Font(bold=True, size=11)
    ws_summary.cell(row=6, column=1, value="Duration:")
    ws_summary.cell(row=6, column=2, value=stats["Duration_Str"])
    ws_summary.cell(row=7, column=1, value="Total Samples:")
    ws_summary.cell(row=7, column=2, value=stats["Total_Samples"])
    ws_summary.cell(row=8, column=1, value="Sample Rate:")
    ws_summary.cell(row=8, column=2, value=f"{stats['Sample_Rate_Hz']:.1f} Hz")
    ws_summary.cell(row=9, column=1, value="Total Energy:")
    ws_summary.cell(row=9, column=2, value=f"{stats['Channels'].get('Total_Current_A', {}).get('ah', 0):.4f} Ah ({stats.get('Power', {}).get('wh', 0):.2f} Wh)")

    # Channel Guide Table
    ws_summary.cell(row=12, column=1, value="Telemetry Channels & Hardware Mapping").font = Font(bold=True, size=11)
    guide_headers = ["Channel / Column", "Hardware Module", "Measurement Type", "Range / Spec"]
    for col_idx, h in enumerate(guide_headers, 1):
        c = ws_summary.cell(row=13, column=col_idx, value=h)
        c.font = Font(bold=True, color="FFFFFF")
        c.fill = PatternFill(start_color="1E40AF", end_color="1E40AF", fill_type="solid")

    guide_rows = [
        ("INA1_A, INA1_V, INA1_W", "INA228 Shunt #1 (0x40)", "Primary Bus Voltage & Shunt Current", "0-85V, Bidirectional"),
        ("INA2_A, INA2_V, INA2_W", "INA228 Shunt #2 (0x41)", "Secondary Bus Voltage & Current", "0-85V, Bidirectional"),
        ("DHAB_Low_A", "LEM DHAB S/157 (Low Range)", "Channel 1 High-Sensitivity Current", "+/- 50A (40 mV/A)"),
        ("DHAB_High_A", "LEM DHAB S/157 (High Range)", "Channel 2 High-Current Range", "+/- 500A (4 mV/A)"),
        ("DHAB_A", "LEM DHAB Combined", "Firmware Auto-Selected Best Range", "Seamless 0-500A"),
        ("HO100_A", "LEM HO-100-S Hall Clamp", "High-Speed Open Loop Hall Current", "+/- 100A Peak"),
        ("Total_Current_A", "Calculated Total Load", "Sum of Monitored Sensor Channels", "System Aggregate"),
        ("Total_Power_W", "Calculated Power", "System Power Draw", "Watts (W)"),
        ("Batt_V, Batt_Pct", "MAX17048 Fuel Gauge (0x36)", "Internal 18650 Battery State", "3.0V - 4.2V, 0-100%")
    ]
    for row_idx, r in enumerate(guide_rows, 14):
        for col_idx, val in enumerate(r, 1):
            ws_summary.cell(row=row_idx, column=col_idx, value=val)

    # 2. Sheet 2: Raw Telemetry Data
    ws_data = wb.create_sheet(title="Telemetry Data")
    for r in dataframe_to_rows(df, index=False, header=True):
        ws_data.append(r)

    # Header styling for Data Sheet
    for col_idx in range(1, len(df.columns) + 1):
        c = ws_data.cell(row=1, column=col_idx)
        c.font = Font(bold=True, color="FFFFFF")
        c.fill = PatternFill(start_color="334155", end_color="334155", fill_type="solid")

    # Add Native Current Chart in Excel
    num_rows = min(len(df) + 1, 1000)
    chart_current = LineChart()
    chart_current.title = "Current Waveforms (Amps)"
    chart_current.style = 13
    chart_current.y_axis.title = "Current (A)"
    chart_current.x_axis.title = "Sample Index"
    chart_current.width = 18
    chart_current.height = 10

    col_names = list(df.columns)
    for c_target in ["Total_Current_A", "INA1_A", "DHAB_Low_A", "DHAB_High_A", "HO100_A"]:
        if c_target in col_names:
            c_idx = col_names.index(c_target) + 1
            data_ref = Reference(ws_data, min_col=c_idx, min_row=1, max_row=num_rows)
            chart_current.add_data(data_ref, titles_from_data=True)

    ws_summary.add_chart(chart_current, "F5")

    wb.save(output_xlsx)
    print(f"  [OK] Exported Excel Workbook with Charts -> {output_xlsx}")
    return output_xlsx

def launch_gui_viewer(csv_path=None, invert_dhab=False, invert_ho100=False):
    """Launches an interactive desktop viewer using Tkinter & Matplotlib."""
    import tkinter as tk
    from tkinter import ttk, filedialog, messagebox
    import matplotlib.pyplot as plt
    from matplotlib.backends.backend_tkagg import FigureCanvasTkAgg, NavigationToolbar2Tk

    root = tk.Tk()
    root.title("VAM Multi-Channel Telemetry Graph Viewer")
    root.geometry("1160x760")
    root.configure(bg="#0f172a")

    current_file = [csv_path]
    raw_df = [None]
    current_df = [None]

    var_inv_dhab = tk.BooleanVar(value=invert_dhab)
    var_inv_ho100 = tk.BooleanVar(value=invert_ho100)

    # Top Control Bar
    ctrl_frame = tk.Frame(root, bg="#1e293b", pady=8, padx=12)
    ctrl_frame.pack(fill=tk.X, side=tk.TOP)

    lbl_file = tk.Label(ctrl_frame, text="No log loaded", fg="#94a3b8", bg="#1e293b", font=("Segoe UI", 10, "bold"))
    lbl_file.pack(side=tk.LEFT, padx=8)

    # Plot Frame
    plot_frame = tk.Frame(root, bg="#0f172a")
    plot_frame.pack(fill=tk.BOTH, expand=True, padx=10, pady=5)

    fig, (ax_curr, ax_volt) = plt.subplots(2, 1, figsize=(10, 6), sharex=True)
    fig.patch.set_facecolor("#0f172a")
    ax_curr.set_facecolor("#1e293b")
    ax_volt.set_facecolor("#1e293b")

    for ax in (ax_curr, ax_volt):
        ax.tick_params(colors="#94a3b8")
        ax.grid(True, color="#334155", linestyle="--", alpha=0.7)
        for spine in ax.spines.values():
            spine.set_color("#334155")

    ax_curr.set_ylabel("Current (A)", color="#38bdf8", fontsize=11, fontweight="bold")
    ax_volt.set_ylabel("Voltage (V)", color="#facc15", fontsize=11, fontweight="bold")
    ax_volt.set_xlabel("Time (Seconds)", color="#94a3b8", fontsize=11)

    canvas = FigureCanvasTkAgg(fig, master=plot_frame)
    canvas.get_tk_widget().pack(fill=tk.BOTH, expand=True)

    toolbar = NavigationToolbar2Tk(canvas, plot_frame)
    toolbar.update()

    def update_plot():
        df = current_df[0]
        if df is None:
            return

        ax_curr.clear()
        ax_volt.clear()

        for ax in (ax_curr, ax_volt):
            ax.set_facecolor("#1e293b")
            ax.tick_params(colors="#94a3b8")
            ax.grid(True, color="#334155", linestyle="--", alpha=0.7)
            for spine in ax.spines.values():
                spine.set_color("#334155")

        if len(df) == 0:
            ax_curr.set_title("No valid telemetry rows in this CSV file", color="#f87171", fontsize=11)
            fig.tight_layout()
            canvas.draw()
            return

        t = df["Time_Sec"]

        # Plot Currents
        if "Total_Current_A" in df:
            ax_curr.plot(t, df["Total_Current_A"], label="Total Current", color="#c084fc", linewidth=2.0)
        if "INA1_A" in df:
            ax_curr.plot(t, df["INA1_A"], label="INA1 Shunt", color="#38bdf8", linewidth=1.4)
        if "INA2_A" in df:
            ax_curr.plot(t, df["INA2_A"], label="INA2 Shunt", color="#fb923c", linewidth=1.4)
        if "DHAB_Low_A" in df:
            lbl = "DHAB Low (±50A) [INV]" if var_inv_dhab.get() else "DHAB Low (±50A)"
            ax_curr.plot(t, df["DHAB_Low_A"], label=lbl, color="#4ade80", linewidth=1.4)
        if "DHAB_High_A" in df:
            lbl = "DHAB High (±500A) [INV]" if var_inv_dhab.get() else "DHAB High (±500A)"
            ax_curr.plot(t, df["DHAB_High_A"], label=lbl, color="#15803d", linewidth=1.4, linestyle=":")
        elif "DHAB_A" in df:
            lbl = "DHAB Clamp [INV]" if var_inv_dhab.get() else "DHAB Clamp"
            ax_curr.plot(t, df["DHAB_A"], label=lbl, color="#4ade80", linewidth=1.4)
        if "HO100_A" in df:
            lbl = "HO100 Clamp [INV]" if var_inv_ho100.get() else "HO100 Clamp"
            ax_curr.plot(t, df["HO100_A"], label=lbl, color="#f87171", linewidth=1.4)

        ax_curr.set_ylabel("Current (A)", color="#f8fafc", fontsize=11, fontweight="bold")
        ax_curr.legend(loc="upper right", facecolor="#1e293b", edgecolor="#334155", labelcolor="#f8fafc")

        # Plot Voltages
        if "INA1_V" in df:
            ax_volt.plot(t, df["INA1_V"], label="INA1 Bus V", color="#38bdf8", linewidth=1.5)
        if "INA2_V" in df:
            ax_volt.plot(t, df["INA2_V"], label="INA2 Bus V", color="#fb923c", linewidth=1.5)
        if "Batt_V" in df:
            ax_volt.plot(t, df["Batt_V"], label="Feather 18650 Battery", color="#facc15", linewidth=1.5, linestyle="--")

        ax_volt.set_ylabel("Voltage (V)", color="#f8fafc", fontsize=11, fontweight="bold")
        ax_volt.set_xlabel("Time (Seconds)", color="#94a3b8", fontsize=11)
        ax_volt.legend(loc="upper right", facecolor="#1e293b", edgecolor="#334155", labelcolor="#f8fafc")

        fig.tight_layout()
        canvas.draw()

    def recompute_active_df():
        if raw_df[0] is None:
            return
        current_df[0] = apply_polarity_inversion(
            raw_df[0],
            invert_dhab=var_inv_dhab.get(),
            invert_ho100=var_inv_ho100.get(),
        )
        update_plot()

    def open_file_dialog():
        p = filedialog.askopenfilename(
            initialdir=os.path.join(os.path.dirname(__file__), "logs"),
            title="Select CSV Telemetry Log",
            filetypes=[("CSV Log Files", "*.csv"), ("All Files", "*.*")]
        )
        if p:
            load_file(p)

    def load_file(p):
        try:
            df = load_log_file(p)
            raw_df[0] = df
            current_file[0] = p
            skipped = df.attrs.get("skipped_rows", 0)
            skip_info = f", {skipped} corrupt skipped" if skipped > 0 else ""
            lbl_file.config(text=f"Loaded: {os.path.basename(p)} ({len(df)} records{skip_info})")
            recompute_active_df()
        except Exception as e:
            messagebox.showerror("Error", f"Failed to load CSV:\n{e}")

    def on_export_html():
        if current_df[0] is None or current_file[0] is None:
            messagebox.showwarning("Warning", "Please open a log file first.")
            return
        out = export_html_report(current_df[0], current_file[0])
        import webbrowser
        webbrowser.open(os.path.abspath(out))

    def on_export_excel():
        if current_df[0] is None or current_file[0] is None:
            messagebox.showwarning("Warning", "Please open a log file first.")
            return
        out = export_excel_report(current_df[0], current_file[0])
        messagebox.showinfo("Export Complete", f"Excel Workbook created:\n{out}")

    # Buttons
    btn_open = tk.Button(ctrl_frame, text="📁 Open CSV Log", command=open_file_dialog, bg="#3b82f6", fg="white", font=("Segoe UI", 9, "bold"), padx=10, relief=tk.FLAT)
    btn_open.pack(side=tk.LEFT, padx=6)

    btn_html = tk.Button(ctrl_frame, text="🌐 Interactive HTML Report", command=on_export_html, bg="#0d9488", fg="white", font=("Segoe UI", 9, "bold"), padx=10, relief=tk.FLAT)
    btn_html.pack(side=tk.LEFT, padx=6)

    btn_excel = tk.Button(ctrl_frame, text="📊 Export to Excel (.xlsx)", command=on_export_excel, bg="#15803d", fg="white", font=("Segoe UI", 9, "bold"), padx=10, relief=tk.FLAT)
    btn_excel.pack(side=tk.LEFT, padx=6)

    chk_dhab = tk.Checkbutton(
        ctrl_frame, text="Invert DHAB (±)", variable=var_inv_dhab, command=recompute_active_df,
        bg="#1e293b", fg="#4ade80", selectcolor="#0f172a", activebackground="#1e293b", activeforeground="#4ade80",
        font=("Segoe UI", 9, "bold")
    )
    chk_dhab.pack(side=tk.LEFT, padx=8)

    chk_ho = tk.Checkbutton(
        ctrl_frame, text="Invert HO-100 (±)", variable=var_inv_ho100, command=recompute_active_df,
        bg="#1e293b", fg="#f87171", selectcolor="#0f172a", activebackground="#1e293b", activeforeground="#f87171",
        font=("Segoe UI", 9, "bold")
    )
    chk_ho.pack(side=tk.LEFT, padx=4)

    if csv_path and os.path.exists(csv_path):
        load_file(csv_path)

    root.mainloop()

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="VAM Logger - Log Viewer & Analysis")
    parser.add_argument("file", nargs="?", default=None, help="Path to CSV log file")
    parser.add_argument("--html", action="store_true", help="Generate HTML report and exit")
    parser.add_argument("--excel", action="store_true", help="Generate Excel report and exit")
    parser.add_argument("--invert-dhab", action="store_true", help="Invert DHAB polarity")
    parser.add_argument("--invert-ho100", action="store_true", help="Invert HO-100-S polarity")
    args = parser.parse_args()

    target_file = args.file
    if not target_file:
        logs_dir = os.path.join(os.path.dirname(__file__), "logs")
        if os.path.exists(logs_dir):
            files = [os.path.join(logs_dir, f) for f in os.listdir(logs_dir) if f.endswith(".csv")]
            if files:
                target_file = max(files, key=os.path.getmtime)

    if target_file and os.path.exists(target_file):
        if args.html:
            df = apply_polarity_inversion(load_log_file(target_file), args.invert_dhab, args.invert_ho100)
            out = export_html_report(df, target_file)
            print(f"Report ready: {out}")
            sys.exit(0)
        elif args.excel:
            df = apply_polarity_inversion(load_log_file(target_file), args.invert_dhab, args.invert_ho100)
            out = export_excel_report(df, target_file)
            print(f"Excel ready: {out}")
            sys.exit(0)

    launch_gui_viewer(target_file, invert_dhab=args.invert_dhab, invert_ho100=args.invert_ho100)
