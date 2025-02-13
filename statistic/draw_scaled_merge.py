import os
import matplotlib.pyplot as plt

# === Configuration ===
NUM_ENV = 7    # Environments 1..7 (files: results_summary-1.txt ... results_summary-7.txt, folders: time_log-1, etc.)
NUM_SMP = 32   # SMP values 1..32
BASE_DIR = "./profile-2"  # Adjust as needed
THRESHOLD = 1e8  # Threshold for bogus 'total' values in time logs

# === Helper functions for summary files ===
def parse_summary_file(env):
    """
    Reads results_summary-N.txt for the given environment (env) and returns a dictionary
    mapping SMP (int) to a tuple of summary values:
      (real_boot_time, times_called, predicted_ns_per_call, predict_sec,
       scale_factor, total_clocksource_ns, percentage)
    """
    summary_path = os.path.join(BASE_DIR, f"results_summary-{env}.txt")
    data = {}
    with open(summary_path, "r") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split()
            if parts[0] == "SMP":  # skip header
                continue
            if len(parts) >= 10:
                try:
                    smp = int(parts[0])
                    real_boot_time = float(parts[1])
                    times_called = float(parts[2])
                    predicted_ns_per_call = float(parts[3])
                    predict_sec = float(parts[4])
                    scale_factor = float(parts[5])
                    total_clocksource_ns = float(parts[6])
                    percentage_val = float(parts[7])
                    data[smp] = (real_boot_time, times_called, predicted_ns_per_call, predict_sec,
                                 scale_factor, total_clocksource_ns, percentage_val)
                except Exception:
                    continue
    return data

# === Helper functions for time-log files ===
def parse_ns_per_call_time_log(time_log_path):
    """
    Reads a time log file (with lines like: "diff: <diff_value>, total: <total_value>")
    and returns a list of real ns_per_call values computed as:
         (total / 2) / 1e6
    Measurements with a 'total' value greater than THRESHOLD are skipped.
    """
    ns_values = []
    with open(time_log_path, "r") as f:
        for line in f:
            line = line.strip()
            if line.startswith("diff:"):
                try:
                    parts = line.split(',')
                    # Example parts: ["diff: 273002121", " total: 42000329"]
                    total_str = parts[1].split(':')[1].strip()
                    total_val = float(total_str)
                    if total_val > THRESHOLD:
                        continue
                    ns_val = (total_val / 2) / 1_000_000
                    ns_values.append(ns_val)
                except (IndexError, ValueError):
                    continue
    return ns_values

def parse_percentage_time_log(time_log_path):
    """
    Reads a time log file (with lines like: "diff: <diff_value>, total: <total_value>")
    and returns a list of percentage values computed as (total/diff) per line.
    Measurements with a 'total' value greater than THRESHOLD are skipped.
    """
    perc_values = []
    with open(time_log_path, "r") as f:
        for line in f:
            line = line.strip()
            if line.startswith("diff:"):
                try:
                    parts = line.split(',')
                    diff_str = parts[0].split(':')[1].strip()
                    total_str = parts[1].split(':')[1].strip()
                    diff_val = float(diff_str)
                    total_val = float(total_str)
                    if total_val > THRESHOLD:
                        continue
                    if diff_val != 0:
                        perc_val = total_val / diff_val
                        perc_values.append(perc_val)
                except (IndexError, ValueError):
                    continue
    return perc_values

# === Data collection ===
# For each metric, we build a dictionary mapping environment -> list (of length NUM_SMP, one per SMP)
# Metrics from summary files:
#   - diff_boot_time = (predict_sec - real_boot_time)
#   - predict_boot_time = predict_sec
#   - predicted_ns_call   = predicted_ns_per_call
#   - real_boot_time      = real_boot_time
#   - scale_factor        = scale_factor
#   - times_called_cs     = times_called
#   - total_clock_ns      = total_clocksource_ns
#
# Metrics from time log files:
#   - avg_percentage      = average of (total/diff) computed per SMP
#   - avg_real_ns_call    = average real ns_per_call computed as above
#   - diff_ns_call        = (avg_real_ns_call - predicted_ns_call)
diff_boot_time    = {}  # Metric 1
predict_boot_time = {}  # Metric 3
predict_ns_call   = {}  # Metric 4
real_boot_time    = {}  # Metric 6
scale_factor      = {}  # Metric 7
times_called_cs   = {}  # Metric 8
total_clock_ns    = {}  # Metric 9

avg_percentage    = {}  # Metric 2 (from time logs)
avg_real_ns_call  = {}  # Intermediate for Metric 5
diff_ns_call      = {}  # Metric 5

# Loop over each environment (1..7)
for env in range(1, NUM_ENV + 1):
    summary = parse_summary_file(env)
    # Initialize lists (one element per SMP 1..32)
    diff_boot_time[env]    = []
    predict_boot_time[env] = []
    predict_ns_call[env]   = []
    real_boot_time[env]    = []
    scale_factor[env]      = []
    times_called_cs[env]   = []
    total_clock_ns[env]    = []
    avg_percentage[env]    = []
    avg_real_ns_call[env]  = []
    diff_ns_call[env]      = []
    
    for smp in range(1, NUM_SMP + 1):
        # Get summary values if available; otherwise use 0
        if smp in summary:
            (rbt, tc, pred_ns, pred_sec, sf, total_cs, _) = summary[smp]
            diff_bt = pred_sec - rbt
            diff_boot_time[env].append(diff_bt)
            predict_boot_time[env].append(pred_sec)
            predict_ns_call[env].append(pred_ns)
            real_boot_time[env].append(rbt)
            scale_factor[env].append(sf)
            times_called_cs[env].append(tc)
            total_clock_ns[env].append(total_cs)
        else:
            diff_boot_time[env].append(0.0)
            predict_boot_time[env].append(0.0)
            predict_ns_call[env].append(0.0)
            real_boot_time[env].append(0.0)
            scale_factor[env].append(0.0)
            times_called_cs[env].append(0.0)
            total_clock_ns[env].append(0.0)
        
        # Process the time log file for this SMP in this environment.
        time_log_path = os.path.join(BASE_DIR, f"time_log-{env}", f"time_log_{smp}.txt")
        if os.path.exists(time_log_path):
            perc_vals = parse_percentage_time_log(time_log_path)
            ns_vals   = parse_ns_per_call_time_log(time_log_path)
        else:
            perc_vals = []
            ns_vals = []
        
        # Average percentage (should be around 0.1–0.2, etc.)
        if perc_vals:
            avg_perc = sum(perc_vals) / len(perc_vals)
        else:
            avg_perc = 0.0
        avg_percentage[env].append(avg_perc)
        
        # Average real ns_per_call computed from time-log file
        if ns_vals:
            avg_ns = sum(ns_vals) / len(ns_vals)
        else:
            avg_ns = 0.0
        avg_real_ns_call[env].append(avg_ns)
        
        # Metric 5: Difference ns_per_call = (average real ns_per_call from time logs) - (predicted ns_per_call from summary)
        pred_val = predict_ns_call[env][-1] if predict_ns_call[env] else 0.0
        diff_ns_call[env].append(avg_ns - pred_val)

# === Plotting the 9 Comparison Figures ===
output_dir = os.path.join(BASE_DIR, "comparison_plots")
os.makedirs(output_dir, exist_ok=True)

smp_range = list(range(1, NUM_SMP + 1))
colors = ['blue', 'orange', 'green', 'red', 'purple', 'brown', 'olive']  # one color per environment

# 1. diff_boot_time: (predicted boot time - real boot time)
plt.figure(figsize=(10, 6))
for env in range(1, NUM_ENV + 1):
    plt.plot(smp_range, diff_boot_time[env], color=colors[env - 1], linestyle='-', marker = 'o', label=f"Env {env}")
plt.title("Diff Boot Time (Predicted Boot Time - Real Boot Time)")
plt.xlabel("SMP")
plt.ylabel("Time Difference (sec)")
plt.legend()
plt.grid(True)
plt.savefig(os.path.join(output_dir, "diff_boot_time.png"), dpi=150)
plt.close()

# 2. Percentage of clocksource (average per SMP from time logs)
plt.figure(figsize=(10, 6))
for env in range(1, NUM_ENV + 1):
    plt.plot(smp_range, avg_percentage[env], color=colors[env - 1], linestyle='-', marker = 'o', label=f"Env {env}")
plt.title("Average Percentage of Clocksource (per SMP)")
plt.xlabel("SMP")
plt.ylabel("Average Percentage (total/diff)")
plt.legend()
plt.grid(True)
plt.savefig(os.path.join(output_dir, "percentage_clocksource.png"), dpi=150)
plt.close()

# 3. Predicted boot time (predict_sec from summary)
plt.figure(figsize=(10, 6))
for env in range(1, NUM_ENV + 1):
    plt.plot(smp_range, predict_boot_time[env], color=colors[env - 1], linestyle='-', marker = 'o', label=f"Env {env}")
plt.title("Predicted Boot Time")
plt.xlabel("SMP")
plt.ylabel("Predicted Boot Time (sec)")
plt.legend()
plt.grid(True)
plt.savefig(os.path.join(output_dir, "predicted_boot_time.png"), dpi=150)
plt.close()

# 4. Predicted ns_per_call (from summary, column 4)
plt.figure(figsize=(10, 6))
for env in range(1, NUM_ENV + 1):
    plt.plot(smp_range, predict_ns_call[env], color=colors[env - 1], linestyle='-', marker = 'o', label=f"Env {env}")
plt.title("Predicted ns_per_call")
plt.xlabel("SMP")
plt.ylabel("Predicted ns_per_call")
plt.legend()
plt.grid(True)
plt.savefig(os.path.join(output_dir, "predicted_ns_per_call.png"), dpi=150)
plt.close()

# 5. diff ns_per_call: (avg real ns_per_call from time logs - predicted ns_per_call)
plt.figure(figsize=(10, 6))
for env in range(1, NUM_ENV + 1):
    plt.plot(smp_range, diff_ns_call[env], color=colors[env - 1], linestyle='-', marker = 'o', label=f"Env {env}")
plt.title("Difference ns_per_call (Real - Predicted)")
plt.xlabel("SMP")
plt.ylabel("ns_per_call Difference")
plt.legend()
plt.grid(True)
plt.savefig(os.path.join(output_dir, "diff_ns_per_call.png"), dpi=150)
plt.close()

# 6. Real boot time (from summary, column 2)
plt.figure(figsize=(10, 6))
for env in range(1, NUM_ENV + 1):
    plt.plot(smp_range, real_boot_time[env], color=colors[env - 1], linestyle='-', marker = 'o', label=f"Env {env}")
plt.title("Real Boot Time")
plt.xlabel("SMP")
plt.ylabel("Real Boot Time (sec)")
plt.legend()
plt.grid(True)
plt.savefig(os.path.join(output_dir, "real_boot_time.png"), dpi=150)
plt.close()

# 7. Scale factor (from summary, column 6)
plt.figure(figsize=(10, 6))
for env in range(1, NUM_ENV + 1):
    plt.plot(smp_range, scale_factor[env], color=colors[env - 1], linestyle='-', marker = 'o', label=f"Env {env}")
plt.title("Scale Factor")
plt.xlabel("SMP")
plt.ylabel("Scale Factor")
plt.legend()
plt.grid(True)
plt.savefig(os.path.join(output_dir, "scale_factor.png"), dpi=150)
plt.close()

# 8. Times called clocksource (from summary, column 3)
plt.figure(figsize=(10, 6))
for env in range(1, NUM_ENV + 1):
    plt.plot(smp_range, times_called_cs[env], color=colors[env - 1], linestyle='-', marker = 'o', label=f"Env {env}")
plt.title("Times Called Clocksource")
plt.xlabel("SMP")
plt.ylabel("Times Called")
plt.legend()
plt.grid(True)
plt.savefig(os.path.join(output_dir, "times_called_clocksource.png"), dpi=150)
plt.close()

# 9. Total clocksource ns (from summary, column 7)
plt.figure(figsize=(10, 6))
for env in range(1, NUM_ENV + 1):
    plt.plot(smp_range, total_clock_ns[env], color=colors[env - 1], linestyle='-', marker = 'o', label=f"Env {env}")
plt.title("Total Clocksource ns")
plt.xlabel("SMP")
plt.ylabel("Total Clocksource ns")
plt.legend()
plt.grid(True)
plt.savefig(os.path.join(output_dir, "total_clocksource_ns.png"), dpi=150)
plt.close()

print(f"[INFO] Finished producing 9 comparison plots in '{output_dir}'.")
