#!/usr/bin/env python3
import os
import re
import matplotlib.pyplot as plt

# === Configuration ===
BASE_DIR = "./profile-3"          # Base directory where your logs are stored.
NUM_ENV = 7                       # Number of environments (logs-1, logs-2, ..., logs-7)
NUM_SMP = 32                      # SMP values 1..32
OUT_DIR = os.path.join(BASE_DIR, "timer_metrics_plots")
os.makedirs(OUT_DIR, exist_ok=True)

# Regular expressions to extract the desired lines.
# Boot time line example:
#   [SEMU LOG]: Boot time: 3.59699 seconds, called 239937385 times semu_timer_clocksource
boot_re = re.compile(r"Boot time:\s*([0-9.]+)\s+seconds,\s+called\s+([0-9]+)\s+times", re.IGNORECASE)

# Offset line example:
#   [SEMU LOG]: timer->begin: 29344993606427, real_ticks: 29345227607119, boot_ticks: 29345773402928, offset: -545795809
offset_re = re.compile(r"offset:\s*(-?[0-9]+)", re.IGNORECASE)

# Function to remove ANSI escape codes from a line
def remove_ansi_codes(s):
    ansi_escape = re.compile(r'\x1b\[[0-9;]*m')
    return ansi_escape.sub('', s)

# Data dictionary: keys are SMP (1..NUM_SMP) and value is another dict mapping env (1..NUM_ENV)
# to a dictionary with keys: "boot", "called", "offset".
data = {}
for smp in range(1, NUM_SMP + 1):
    data[smp] = {}
    for env in range(1, NUM_ENV + 1):
        # Pre-initialize with default values (0.0 for boot time, 0 for called and offset)
        data[smp][env] = {"boot": 0.0, "called": 0, "offset": 0}
        log_dir = os.path.join(BASE_DIR, f"logs-{env}")
        logfile = os.path.join(log_dir, f"emulator_SMP_{smp}.log")
        if not os.path.exists(logfile):
            print(f"Warning: File not found: {logfile} (using default zeros)")
            continue

        with open(logfile, "r") as f:
            content = f.read()
        content = remove_ansi_codes(content)

        boot_match = boot_re.search(content)
        if boot_match:
            boot_time = float(boot_match.group(1))
            times_called = int(boot_match.group(2))
        else:
            print(f"Warning: Could not parse boot time from {logfile}")
            boot_time = 0.0
            times_called = 0

        offset_match = offset_re.search(content)
        if offset_match:
            offset_val = int(offset_match.group(1))
        else:
            print(f"Warning: Could not parse offset from {logfile}")
            offset_val = 0

        data[smp][env] = {
            "boot": boot_time,
            "called": times_called,
            "offset": offset_val
        }

# For each SMP, create one figure with three vertical bar charts (Boot time, Called times, Offset).
for smp in range(1, NUM_SMP + 1):
    # For environments 1 to NUM_ENV, extract values from data[smp]
    boot_times = []
    called_times = []
    offsets = []
    env_labels = []
    for env in range(1, NUM_ENV + 1):
        env_labels.append(f"Env {env}")
        boot_times.append(data[smp].get(env, {"boot": 0.0})["boot"])
        called_times.append(data[smp].get(env, {"called": 0})["called"])
        offsets.append(data[smp].get(env, {"offset": 0})["offset"])

    # Convert called_times to millions (for better scale)
    called_times_m = [ct / 1e6 for ct in called_times]

    # Create a figure with 3 subplots (vertical)
    fig, axs = plt.subplots(nrows=3, ncols=1, figsize=(8, 10))
    fig.suptitle(f"SMP {smp} Timer Metrics", fontsize=16)

    # Plot Boot Time
    axs[0].bar(env_labels, boot_times, color='skyblue')
    axs[0].set_ylabel("Boot Time (s)")
    axs[0].set_title("Boot Time")
    for i, v in enumerate(boot_times):
        axs[0].text(i, v, f"{v:.3f}", ha="center", va="bottom", fontsize=8)

    # Plot Called Times (in millions)
    axs[1].bar(env_labels, called_times_m, color='lightgreen')
    axs[1].set_ylabel("Called Times (M)")
    axs[1].set_title("Called Times to semu_timer_clocksource")
    for i, v in enumerate(called_times_m):
        axs[1].text(i, v, f"{v:.2f}", ha="center", va="bottom", fontsize=8)

    # Plot Offset (in nanoseconds)
    axs[2].bar(env_labels, offsets, color='salmon')
    axs[2].set_ylabel("Offset (ns)")
    axs[2].set_title("Timer Offset")
    for i, v in enumerate(offsets):
        axs[2].text(i, v, f"{v}", ha="center", va="bottom", fontsize=8)

    plt.tight_layout(rect=[0, 0, 1, 0.95])
    outfile = os.path.join(OUT_DIR, f"SMP_{smp}_timer_metrics.png")
    plt.savefig(outfile, dpi=150)
    plt.close(fig)
    print(f"Saved figure for SMP {smp} to {outfile}")

print(f"[INFO] Finished producing timer metrics figures in '{OUT_DIR}'.")
