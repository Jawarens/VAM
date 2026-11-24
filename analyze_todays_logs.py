import glob, os, pandas as pd

def read_csv_auto(path):
    with open(path, 'r', encoding='utf-8-sig', errors='replace') as f:
        lines = [f.readline() for _ in range(4)]
    header = lines[0] if lines else ''
    sep = ';' if header.count(';') >= header.count(',') else ','
    decimal = ',' if (sep == ';' and any(',' in ln.split(';')[1] for ln in lines[1:] if ';' in ln)) else '.'
    df = pd.read_csv(path, sep=sep, decimal=decimal, on_bad_lines='skip', low_memory=False)
    for c in df.columns:
        if c != 'Timestamp_ISO':
            df[c] = pd.to_numeric(df[c], errors='coerce')
    return df

logs = sorted(glob.glob('sd-logger/logs/LOG_20261028_*.csv'))
for log in logs:
    df = read_csv_auto(log)
    print(f"=== {os.path.basename(log)} (Rows: {len(df)}) ===")
    print(f"Time: {df['Timestamp_ISO'].iloc[0]} -> {df['Timestamp_ISO'].iloc[-1]}")
    print(f"INA1_V: min={df['INA1_V'].min():.4f}, max={df['INA1_V'].max():.4f}, mean={df['INA1_V'].mean():.4f}")
    print(f"INA1_A: min={df['INA1_A'].min():.4f}, max={df['INA1_A'].max():.4f}, mean={df['INA1_A'].mean():.4f}")
    print(f"INA2_V: min={df['INA2_V'].min():.4f}, max={df['INA2_V'].max():.4f}, mean={df['INA2_V'].mean():.4f}")
    print(f"INA2_A: min={df['INA2_A'].min():.4f}, max={df['INA2_A'].max():.4f}, mean={df['INA2_A'].mean():.4f}")
    print(f"Batt_V: min={df['Batt_V'].min():.2f}, max={df['Batt_V'].max():.2f}, mean={df['Batt_V'].mean():.2f}")
    print(f"HO100_A: min={df['HO100_A'].min():.2f}, max={df['HO100_A'].max():.2f}")
    print(f"DHAB_A:  min={df['DHAB_A'].min():.2f}, max={df['DHAB_A'].max():.2f}")
    print()
