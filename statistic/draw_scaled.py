import os
import matplotlib.pyplot as plt

# -------------------------------
# Updated parse functions with filtering of bogus data.
# -------------------------------

def parse_ns_per_call_time_log(time_log_path):
    """
    Parses a time log file whose lines are formatted as:
       diff: <diff_value>, total: <total_value>
    
    Returns a list of Real ns_per_call values computed as:
         real_ns_per_call = (total / 2) / 1e6
    Filters out measurements where total is far too high.
    """
    ns_values = []
    # Set a threshold above which total is considered bogus.
    THRESHOLD = 1e8  
    with open(time_log_path, "r") as f:
        for line in f:
            line = line.strip()
            if line.startswith("diff:"):
                try:
                    parts = line.split(',')
                    # parts[0]: "diff: <value>", parts[1]: " total: <value>"
                    total_str = parts[1].split(':')[1].strip()
                    total_val = float(total_str)
                    # If total is above our threshold, skip this measurement.
                    if total_val > THRESHOLD:
                        continue
                    # Compute real ns_per_call: divide total by 2 (because of 2 calls per test)
                    # and then convert from ns to "ns per call" units.
                    ns_val = (total_val / 2) / 1_000_000
                    ns_values.append(ns_val)
                except (IndexError, ValueError):
                    continue
    return ns_values

def parse_percentage_time_log(time_log_path):
    """
    Parses a time log file whose lines are formatted as:
       diff: <diff_value>, total: <total_value>
    
    Returns a list of Real percentage values computed as (total/diff).
    Filters out measurements where total is far too high.
    """
    perc_values = []
    THRESHOLD = 1e8  # Threshold for bogus total values.
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
                    # If the total is absurdly high, skip this measurement.
                    if total_val > THRESHOLD:
                        continue
                    if diff_val != 0:
                        perc_val = total_val / diff_val
                        perc_values.append(perc_val)
                except (IndexError, ValueError):
                    continue
    return perc_values

# -------------------------------
# The rest of your script remains the same.
# -------------------------------
def parse_results_summary(summary_path):
    """
    Reads a results_summary-N.txt file and returns a dict mapping SMP to its predicted ns_per_call.
    
    Expected columns (1-based):
      1: SMP
      2: real_boot_time
      3: times_called
      4: ns_per_call         <-- Predicted ns_per_call (we use this)
      5: predict_sec
      6: scale_factor
      7: total_clocksource_ns
      8: percentage
      9: real_ns_per_call
      10: diff_ns_per_call
    """
    smp_info = {}
    with open(summary_path, "r") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue

            parts = line.split()
            # Skip header line if present
            if parts[0] == "SMP":
                continue
            if len(parts) >= 10:
                try:
                    smp_val = int(parts[0])
                    predicted_ns = float(parts[3])
                    smp_info[smp_val] = predicted_ns
                except ValueError:
                    continue
    return smp_info

# (Other functions and original plotting code remain unchanged.)
# ...
# For example, the global plots and per-SMP plotting code already in your script
# will now use the updated parse_ns_per_call_time_log() and parse_percentage_time_log()
# functions that ignore any measurement where total > THRESHOLD.

def main():
    base_statistic_dir = "./profile-2"  # Base folder for logs & summaries
    num_summaries = 7                   # results_summary-1.txt ... results_summary-7.txt
    num_smp = 32                        # Each summary covers SMP=1..32
    
    # (Your existing per-environment (per-SMP) plots code here.)
    for i in range(1, num_summaries + 1):
        summary_file = os.path.join(base_statistic_dir, f"results_summary-{i}.txt")
        time_log_dir = os.path.join(base_statistic_dir, f"time_log-{i}")
        output_dir   = os.path.join(base_statistic_dir, f"plots-{i}")
        os.makedirs(output_dir, exist_ok=True)
        
        smp_data = parse_results_summary(summary_file)
        print(f"[INFO] Parsed {len(smp_data)} SMP entries from {summary_file}")
        
        avg_ns_list = []    # for ns_per_call
        avg_perc_list = []  # for percentage
        
        for smp in range(1, num_smp + 1):
            if smp not in smp_data:
                print(f"[WARNING] SMP {smp} not found in {summary_file}. Skipping.")
                avg_ns_list.append(0.0)
                avg_perc_list.append(0.0)
                continue
            
            predicted_ns = smp_data[smp]
            time_log_path = os.path.join(time_log_dir, f"time_log_{smp}.txt")
            if not os.path.exists(time_log_path):
                print(f"[WARNING] File not found: {time_log_path}")
                avg_ns_list.append(0.0)
                avg_perc_list.append(0.0)
                continue
            
            # --- Plot for ns_per_call ---
            ns_values = parse_ns_per_call_time_log(time_log_path)
            if ns_values:
                avg_ns = sum(ns_values) / len(ns_values)
                plt.figure(figsize=(8, 4))
                plt.plot(ns_values, linestyle='-', label='Real ns_per_call')
                plt.axhline(y=predicted_ns, color='red', linestyle='--',
                            label=f'Predicted ns_per_call ({predicted_ns:.3f})')
                plt.axhline(y=avg_ns, color='brown', linestyle='--',
                            label=f'Average Real ns_per_call ({avg_ns:.3f})')
                plt.title(f"SMP {smp} - Real ns_per_call vs. Predicted")
                plt.xlabel("Measurement index")
                plt.ylabel("ns_per_call")
                plt.legend()
                plot_ns_path = os.path.join(output_dir, f"time_log_{smp}_ns_per_call.png")
                plt.savefig(plot_ns_path, dpi=150)
                plt.close()
            else:
                print(f"[WARNING] No valid ns_per_call data in {time_log_path}")
                avg_ns = 0.0
            avg_ns_list.append(avg_ns)
            
            # --- Plot for Real Percentage ---
            perc_values = parse_percentage_time_log(time_log_path)
            if perc_values:
                plt.figure(figsize=(8, 4))
                plt.plot(perc_values, linestyle='-', color='orange', label='Real Percentage')
                avg_perc = sum(perc_values) / len(perc_values)
                plt.axhline(y=avg_perc, color='purple', linestyle='--', label=f'Average ({avg_perc:.4f})')
                plt.title(f"SMP {smp} - Real Percentage per measurement")
                plt.xlabel("Measurement index")
                plt.ylabel("Real Percentage")
                plt.legend()
                plot_perc_path = os.path.join(output_dir, f"time_log_{smp}_percentage.png")
                plt.savefig(plot_perc_path, dpi=150)
                plt.close()
            else:
                print(f"[WARNING] No valid percentage data in {time_log_path}")
                avg_perc = 0.0
            avg_perc_list.append(avg_perc)
        
        # Trend plots as before ...
        plt.figure(figsize=(8, 4))
        smp_indices = list(range(1, num_smp + 1))
        plt.plot(smp_indices, avg_ns_list, linestyle='-', color='blue')
        plt.title(f"Average Real ns_per_call across SMP (Set {i})")
        plt.xlabel("SMP")
        plt.ylabel("Average Real ns_per_call")
        trend_ns_path = os.path.join(output_dir, "ns_per_call_averages_trend.png")
        plt.savefig(trend_ns_path, dpi=150)
        plt.close()
        
        plt.figure(figsize=(8, 4))
        plt.plot(smp_indices, avg_perc_list, linestyle='-', color='orange')
        plt.title(f"Average Real Percentage across SMP (Set {i})")
        plt.xlabel("SMP")
        plt.ylabel("Average Real Percentage")
        trend_perc_path = os.path.join(output_dir, "percentage_averages_trend.png")
        plt.savefig(trend_perc_path, dpi=150)
        plt.close()
        
        print(f"[INFO] Finished set {i}, results in {output_dir}")
    
    # (Global plots code remains unchanged, and will benefit from the filtering in the above functions.)
    # ...
    
if __name__ == "__main__":
    main()
    
