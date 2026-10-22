import argparse
import sys
import os
import pandas as pd
import openpyxl
from openpyxl.styles import Font, PatternFill, Alignment, Border, Side
from openpyxl.chart import LineChart, Reference

def analyze_telemetry(csv_path, output_excel, threshold_a=5.0):
    print(f"[*] Loading Multi-Channel CSV telemetry: {csv_path}")
    df = pd.read_csv(csv_path)

    total_samples = len(df)
    start_time = df['Timestamp_ISO'].iloc[0] if 'Timestamp_ISO' in df else "N/A"
    end_time = df['Timestamp_ISO'].iloc[-1] if 'Timestamp_ISO' in df else "N/A"

    ina_curr = df['INA_Current_A'] if 'INA_Current_A' in df else pd.Series([0])
    dhab_curr = df['DHAB_Current_A'] if 'DHAB_Current_A' in df else pd.Series([0])
    ho100_curr = df['HO100_Current_A'] if 'HO100_Current_A' in df else pd.Series([0])
    bus_v = df['Bus_Voltage_V'] if 'Bus_Voltage_V' in df else pd.Series([0])
    tot_p = df['Total_Power_W'] if 'Total_Power_W' in df else (ina_curr + dhab_curr + ho100_curr) * bus_v

    # Peak & High Draw Filtering on DHAB or Total
    df['Is_High_Draw'] = (dhab_curr >= threshold_a) | (ho100_curr >= threshold_a) | (ina_curr >= threshold_a)
    high_draw_df = df[df['Is_High_Draw']].copy()

    # Create Excel
    wb = openpyxl.Workbook()
    ws_summary = wb.active
    ws_summary.title = "Executive Summary"

    header_fill = PatternFill(start_color="1F4E79", end_color="1F4E79", fill_type="solid")
    header_font = Font(name="Segoe UI", size=11, bold=True, color="FFFFFF")
    title_font = Font(name="Segoe UI", size=16, bold=True, color="1F4E79")
    bold_font = Font(name="Segoe UI", size=10, bold=True)
    regular_font = Font(name="Segoe UI", size=10)
    alert_fill = PatternFill(start_color="FFC7CE", end_color="FFC7CE", fill_type="solid")
    alert_font = Font(name="Segoe UI", size=10, color="9C0006", bold=True)
    border_thin = Border(left=Side(style='thin', color='D9D9D9'), right=Side(style='thin', color='D9D9D9'), top=Side(style='thin', color='D9D9D9'), bottom=Side(style='thin', color='D9D9D9'))

    ws_summary['A1'] = "Multi-Channel Telemetry & Inrush Report (INA228 + DHAB S/157 + HO-100-S)"
    ws_summary['A1'].font = title_font
    ws_summary['A2'] = f"Source Log: {os.path.basename(csv_path)} | Start: {start_time} | End: {end_time}"
    ws_summary['A2'].font = Font(name="Segoe UI", size=10, italic=True, color="595959")

    headers = ["Metric / Sensor Channel", "Max (Peak)", "Average", "Min (Idle)", "Unit"]
    for col_num, h in enumerate(headers, 1):
        cell = ws_summary.cell(row=4, column=col_num, value=h)
        cell.fill = header_fill
        cell.font = header_font
        cell.alignment = Alignment(horizontal="center")

    summary_rows = [
        ("LEM DHAB S/157 (Main Battery Rail +/-500A)", f"{dhab_curr.max():.2f}", f"{dhab_curr.mean():.2f}", f"{dhab_curr.min():.2f}", "A"),
        ("LEM HO-100-S (Aux Clamp Rail +/-100A)", f"{ho100_curr.max():.2f}", f"{ho100_curr.mean():.2f}", f"{ho100_curr.min():.2f}", "A"),
        ("Texas Instruments INA228 (Precision Shunt)", f"{ina_curr.max():.4f}", f"{ina_curr.mean():.4f}", f"{ina_curr.min():.4f}", "A"),
        ("System DC Bus Voltage", f"{bus_v.max():.2f}", f"{bus_v.mean():.2f}", f"{bus_v.min():.2f}", "V"),
        ("Total Combined System Power", f"{tot_p.max():.2f}", f"{tot_p.mean():.2f}", f"{tot_p.min():.2f}", "W"),
        ("Total Session Samples", total_samples, total_samples, total_samples, "samples"),
        ("High-Draw Event Sample Count", len(high_draw_df), f"{len(high_draw_df)/total_samples*100:.2f}%", "-", f">={threshold_a}A")
    ]

    for r_idx, row_data in enumerate(summary_rows, 5):
        for c_idx, val in enumerate(row_data, 1):
            c = ws_summary.cell(row=r_idx, column=c_idx, value=val)
            c.font = bold_font if c_idx == 1 else regular_font
            c.border = border_thin
            if c_idx == 2 and r_idx in [5, 6] and float(val) >= threshold_a:
                c.fill = alert_fill
                c.font = alert_font

    for col in ws_summary.columns:
        max_len = max(len(str(cell.value or '')) for cell in col)
        col_letter = openpyxl.utils.get_column_letter(col[0].column)
        ws_summary.column_dimensions[col_letter].width = max(max_len + 4, 15)

    # High-Draw Events Sheet
    ws_events = wb.create_sheet(title="High-Draw Events")
    event_headers = ["Timestamp", "DHAB Current (A)", "HO100 Current (A)", "INA Current (A)", "Bus V", "Power (W)", "DHAB Mode"]
    for col_num, h in enumerate(event_headers, 1):
        cell = ws_events.cell(row=1, column=col_num, value=h)
        cell.fill = header_fill
        cell.font = header_font

    for r_idx, (_, row) in enumerate(high_draw_df.iterrows(), 2):
        ws_events.cell(row=r_idx, column=1, value=row.get('Timestamp_ISO', '')).font = regular_font
        c1 = ws_events.cell(row=r_idx, column=2, value=row.get('DHAB_Current_A', 0.0))
        c1.font = alert_font if row.get('DHAB_Current_A', 0.0) >= threshold_a else regular_font
        ws_events.cell(row=r_idx, column=3, value=row.get('HO100_Current_A', 0.0)).font = regular_font
        ws_events.cell(row=r_idx, column=4, value=row.get('INA_Current_A', 0.0)).font = regular_font
        ws_events.cell(row=r_idx, column=5, value=row.get('Bus_Voltage_V', 0.0)).font = regular_font
        ws_events.cell(row=r_idx, column=6, value=row.get('Total_Power_W', 0.0)).font = regular_font
        ws_events.cell(row=r_idx, column=7, value=row.get('DHAB_Range', 'LOW_50A')).font = regular_font

    for col in ws_events.columns:
        max_len = max(len(str(cell.value or '')) for cell in col)
        col_letter = openpyxl.utils.get_column_letter(col[0].column)
        ws_events.column_dimensions[col_letter].width = max(max_len + 3, 16)

    # Multi-Channel Chart
    ws_raw = wb.create_sheet(title="Raw Data (Sampled)")
    raw_cols = ['Timestamp_ISO', 'DHAB_Current_A', 'HO100_Current_A', 'INA_Current_A', 'Bus_Voltage_V']
    for c_idx, col_name in enumerate(raw_cols, 1):
        cell = ws_raw.cell(row=1, column=c_idx, value=col_name)
        cell.fill = header_fill
        cell.font = header_font

    step = max(1, len(df) // 10000)
    sampled_df = df.iloc[::step]
    for r_idx, (_, row) in enumerate(sampled_df.iterrows(), 2):
        for c_idx, col_name in enumerate(raw_cols, 1):
            ws_raw.cell(row=r_idx, column=c_idx, value=row.get(col_name, 0)).font = regular_font

    chart = LineChart()
    chart.title = "Simultaneous 3-Channel Current Comparison (DHAB vs HO-100 vs INA228)"
    chart.style = 13
    chart.y_axis.title = "Current (A)"
    chart.x_axis.title = "Time"
    chart.width = 24
    chart.height = 12

    data = Reference(ws_raw, min_col=2, min_row=1, max_col=4, max_row=len(sampled_df) + 1)
    chart.add_data(data, titles_from_data=True)
    ws_summary.add_chart(chart, "A14")

    wb.save(output_excel)
    print(f"[+] Multi-Channel Excel report generated: {output_excel}")

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Multi-Channel Telemetry Analyzer")
    parser.add_argument("csv_file", help="Path to multi-channel CSV log")
    parser.add_argument("-o", "--output", default="MultiChannel_Telemetry_Report.xlsx", help="Output Excel filename")
    parser.add_argument("-t", "--threshold", type=float, default=5.0, help="Inrush current trigger threshold (A)")
    args = parser.parse_args()

    analyze_telemetry(args.csv_file, args.output, args.threshold)
