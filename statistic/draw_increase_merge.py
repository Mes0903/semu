#!/usr/bin/env python3
import os
import re
import matplotlib.pyplot as plt

# === Configuration ===
BASE_DIR = "./profile-3"           # Base directory containing the logs.
NUM_ENV = 7                        # Environments 1..7 (e.g. logs-1, logs-2, ..., logs-7)
NUM_SMP = 32                       # SMP values 1..32
OUT_FILE = os.path.join(BASE_DIR, "merged_timer_metrics.png")

# Regular expressions to extract metrics:
# Boot time line example (ignoring the "[SEMU LOG]:" prefix):
#   Boot time: 3.59699 seconds, called 239937385 times semu_timer_clocksource
boot_re = re.compile(r"Boot time:\s*([0-9.]+)\s+seconds,\s+called\s+([0-9]+)\s+times", re.IGNORECASE)

# Offset line example:
#   timer->begin: 29344993606427, real_ticks: 29345227607119, boot_ticks: 29345773402928, offset: -545795809
offset_re = re.compile(r"offset:\s*(-?[0-9]+)", re.IGNORECASE)

# Function to remove ANSI escape codes from text.
def remove_ansi_codes(s):
    ansi_escape = re.compile(r'\x1b\[[0-9;]*m')
    return ansi_escape.sub('', s)

# Create data containers.
# For each environment, we create lists (length NUM_SMP) for each metric.
boot_data   = {env: [0.0]*NUM_SMP for env in range(1, NUM_ENV+1)}
called_data = {env: [0]*NUM_SMP for env in range(1, NUM_ENV+1)}
offset_data = {env: [0]*NUM_SMP for env in range(1, NUM_ENV+1)}

# Loop over each environment and each SMP to parse the logs.
for env in range(1, NUM_ENV+1):
    for smp in range(1, NUM_SMP+1):
        # Construct log file path (assumed to be in BASE_DIR/logs-{env}/emulator_SMP_{smp}.log)
        log_dir = os.path.join(BASE_DIR, f"logs-{env}")
        logfile = os.path.join(log_dir, f"emulator_SMP_{smp}.log")
        if not os.path.exists(logfile):
            print(f"Warning: File not found: {logfile}. Using default values (0).")
            continue
        with open(logfile, "r") as f:
            content = f.read()
        content = remove_ansi_codes(content)

        # Search for the boot time line.
        boot_match = boot_re.search(content)
        if boot_match:
            boot_time = float(boot_match.group(1))
            times_called = int(boot_match.group(2))
        else:
            print(f"Warning: Could not parse boot time from {logfile}")
            boot_time = 0.0
            times_called = 0

        # Search for the offset line.
        offset_match = offset_re.search(content)
        if offset_match:
            offset_val = int(offset_match.group(1))
        else:
            print(f"Warning: Could not parse offset from {logfile}")
            offset_val = 0

        boot_data[env][smp-1] = boot_time
        called_data[env][smp-1] = times_called
        offset_data[env][smp-1] = offset_val

# Now create one merged figure with three subplots:
#   Subplot 1: Boot Time vs. SMP (line for each environment)
#   Subplot 2: Called Times vs. SMP (converted to millions), with an extra expected line.
#   Subplot 3: Offset vs. SMP.
x = list(range(1, NUM_SMP+1))
colors = ['blue', 'orange', 'green', 'red', 'purple', 'brown', 'olive']

fig, axs = plt.subplots(nrows=3, ncols=1, figsize=(12, 16))
fig.suptitle("Timer Metrics vs. SMP (Merged across Environments)", fontsize=18)

# Subplot 1: Boot Time (seconds)
for env in range(1, NUM_ENV+1):
    axs[0].plot(x, boot_data[env], marker='o', color=colors[env-1],
                label=f"Env {env}")
axs[0].set_title("Boot Time (seconds)")
axs[0].set_xlabel("SMP")
axs[0].set_ylabel("Boot Time (s)")
axs[0].legend()
axs[0].grid(True)

# Subplot 2: Called Times (in millions)
for env in range(1, NUM_ENV+1):
    # Convert called times to millions.
    called_m = [v/1e6 for v in called_data[env]]
    axs[1].plot(x, called_m, marker='o', color=colors[env-1],
                label=f"Env {env}")
# Draw expected line: y = (2e8 * SMP) converted to millions becomes y = 200 * SMP.
expected = [200 * xi for xi in x]
axs[1].plot(x, expected, linestyle='--', color='black', label="Expected (200 * SMP)")
axs[1].set_title("Called Times to semu_timer_clocksource (Millions)")
axs[1].set_xlabel("SMP")
axs[1].set_ylabel("Called Times (M)")
axs[1].legend()
axs[1].grid(True)

# Subplot 3: Offset (nanoseconds)
for env in range(1, NUM_ENV+1):
    axs[2].plot(x, offset_data[env], marker='o', color=colors[env-1],
                label=f"Env {env}")
axs[2].set_title("Timer Offset (nanoseconds)")
axs[2].set_xlabel("SMP")
axs[2].set_ylabel("Offset (ns)")
axs[2].legend()
axs[2].grid(True)

plt.tight_layout(rect=[0, 0, 1, 0.96])
plt.savefig(OUT_FILE, dpi=150)
plt.close(fig)
print(f"Saved merged line chart to {OUT_FILE}")
