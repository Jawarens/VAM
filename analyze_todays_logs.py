import glob, os, pandas as pd

logs = sorted(glob.glob('sd-logger/logs/LOG_20261028_*.csv'))
for log in logs:
    df = pd.read_csv(log)
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
