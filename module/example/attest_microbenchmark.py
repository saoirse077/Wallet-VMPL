#!/usr/bin/env python3
"""
attest_microbenchmark.py - Phase 4 v2.0 差分认证 Microbenchmark 测试

测量各层认证的冷启动/热启动报告生成时间：
  - Monitor 冷/热启动
  - WAMR Runtime 冷/热启动
  - WASM Module 冷/热启动
  - 函数执行认证（不同 input/output 大小）

前置条件：
  - vmpl.ko 已加载 (insmod vmpl.ko)
  - wallet Python 模块已安装
  - wamr_pal.elf, dummy.manifest, dummy.libos, add.wasm 在当前目录
  - dummy_function.txt 在当前目录（会自动创建）

Usage (inside the guest VM):
  cd /root/module
  insmod vmpl.ko
  make -B -C libwallet/ libwallet.so libwallet.a
  cd python && python3 setup.py install && cd ..
  cd example && python3 attest_microbenchmark.py run
"""

import wallet

import os
import csv
import struct
import statistics
import time
from datetime import datetime
from pathlib import Path
from typing import TypeAlias

FileName: TypeAlias = str | os.PathLike

SCRIPTDIR = Path(os.path.dirname(os.path.realpath(__file__)))

# Configuration
fn_in_out_sizes = [64, 1024, 4096]  # size of function input/output in bytes
repeats = 20

# File paths (same as test_phase4_attest.py)
WAMR_PAL_ELF = "./wamr_pal.elf"
DUMMY_MANIFEST = "./dummy.manifest"
DUMMY_LIBOS = "./dummy.libos"
WASM_FILE = "./add.wasm"
DUMMY_FUNCTION = "./dummy_function.txt"
OUTPUT_SIZE = 4096


def pack_input_load_and_invoke(wasm_bytes, func_name, argv):
    """
    Mode 1: Pack input with WASM bytecode + function name + args.
    Used for the first invocation to load a new module.
    """
    func_name_bytes = func_name.encode("ascii")
    func_name_len = len(func_name_bytes)
    argc = len(argv)

    # Header: wasm_size(4) + func_name_len(4) + argc(2) + reserved(2) = 12 bytes
    header = struct.pack("<IIHH", len(wasm_bytes), func_name_len, argc, 0)

    # Function name (padded to 4-byte alignment)
    name_padded_len = (func_name_len + 3) & ~3
    name_data = func_name_bytes + b"\x00" * (name_padded_len - func_name_len)

    # Arguments (each uint32_t)
    argv_data = b""
    for arg in argv:
        argv_data += struct.pack("<I", arg)

    # WASM bytecode
    payload = header + name_data + argv_data + wasm_bytes

    return payload


def pack_shutdown_signal():
    """
    Mode 3: Pack shutdown signal (wasm_size=0, func_name_len=0).
    Signals VMPL1 to unload module, destroy runtime, and exit cleanly.
    """
    header = struct.pack("<IIHH", 0, 0, 0, 0)
    return header


class Runner:
    def __init__(self):
        pass

    @staticmethod
    def time_function(func):
        start = time.perf_counter_ns()
        func()
        end = time.perf_counter_ns()
        return end - start

    def save_results_to_csv(self, results):
        # Create a unique file name
        timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        output_file = SCRIPTDIR / f"phase4_report_generation_measurements_{timestamp}.csv"

        # Prepare data for CSV
        with open(output_file, "w", newline="") as csvfile:
            writer = csv.writer(csvfile)
            writer.writerow(["Measurement", "Size (bytes)", "Average (ns)", "Median (ns)", "StdDev (ns)"])

            # Write non-function measurements
            for key in results.keys():
                if key != "measure_function":
                    avg = statistics.mean(results[key])
                    med = statistics.median(results[key])
                    stddev = statistics.stdev(results[key]) if len(results[key]) > 1 else 0.0
                    writer.writerow([key, "-", avg, med, stddev])
                else:
                    # Write function measurements for different input/output sizes
                    for size, values in results[key].items():
                        avg = statistics.mean(values)
                        med = statistics.median(values)
                        stddev = statistics.stdev(values) if len(values) > 1 else 0.0
                        writer.writerow([key, size, avg, med, stddev])

        print(f"Results saved to {output_file}")

    def check_files(self):
        """Verify all required files exist."""
        missing = []
        for path, desc in [
            (WAMR_PAL_ELF, "WAMR PAL ELF"),
            (DUMMY_MANIFEST, "Dummy manifest"),
            (DUMMY_LIBOS, "Dummy libos"),
            (WASM_FILE, "WASM module (add.wasm)"),
        ]:
            if not os.path.exists(path):
                missing.append(f"  {desc}: {path}")

        # Create dummy_function.txt if not exists
        if not os.path.exists(DUMMY_FUNCTION):
            with open(DUMMY_FUNCTION, "w") as f:
                f.write("dummy")

        if missing:
            print("ERROR: Missing required files:")
            print("\n".join(missing))
            raise FileNotFoundError("Missing required files for microbenchmark")

    def run(
        self,
        zygote: FileName = WAMR_PAL_ELF,
        manifest: FileName = DUMMY_MANIFEST,
        libos: FileName = DUMMY_LIBOS,
    ):
        print("=" * 70)
        print("Phase 4 v2.0 Attestation Microbenchmark")
        print(f"  Repeats: {repeats}")
        print(f"  Function I/O sizes: {fn_in_out_sizes}")
        print("=" * 70)
        print()

        self.check_files()

        # Read WASM module
        with open(WASM_FILE, "rb") as f:
            wasm_bytes = f.read()
        print(f"  WASM module: {len(wasm_bytes)} bytes from {WASM_FILE}")
        print()

        results = {  # To store measurements
            "measure_monitor_cold": [],
            "measure_monitor_hot": [],
            "measure_wamr_runtime_cold": [],
            "measure_wamr_runtime_hot": [],
            "measure_wasm_module_cold": [],
            "measure_wasm_module_hot": [],
            "measure_function": {size: [] for size in fn_in_out_sizes},
        }

        # NOTE: Wallet-VMPL's PROCESS_STORE delete implementation has a bug:
        # when a Trustlet is deleted, its VMSA page is freed back to the page
        # allocator but the RMP VMSA flag is NOT cleared. When the page is
        # later re-allocated and the allocator tries to zero it, the write
        # fails silently (the page is still marked as VMSA in the RMP),
        # causing the system to hang.
        #
        # Workaround: create Zygote/Trustlet ONCE, reuse across all iterations.
        # Only the microbenchmark ioctl calls are repeated; the process
        # lifecycle (create/invoke/shutdown) happens exactly once.

        with wallet.Wallet() as w:
            # Initialize monitor attestation (cache SNP report)
            w.attest_monitor()

            # Create zygote (WAMR runtime initialization) — once
            zy = w.create_zygote(zygote, manifest, libos)

            # Create trustlet (CoW copy of zygote) — once
            tr = zy.create_trustlet(DUMMY_FUNCTION)

            # invoke_trustlet_bin to load WASM module (triggers WASM measurement)
            input_data = pack_input_load_and_invoke(wasm_bytes, "add", [3, 5])
            tr.invoke_trustlet_bin(input_data, OUTPUT_SIZE)
            module_id = 0  # First loaded module

            print("  Setup complete: Zygote, Trustlet, WASM module loaded.")
            print()

            for i in range(repeats):
                print(f"--- Iteration {i+1}/{repeats} ---")

                # Monitor measurements
                results["measure_monitor_cold"].append(
                    self.time_function(w.measure_monitor_cold))
                results["measure_monitor_hot"].append(
                    self.time_function(w.measure_monitor_hot))

                # WAMR Runtime cold: prepare + measure
                zy.prepare_measure_wamr_runtime_cold()
                results["measure_wamr_runtime_cold"].append(
                    self.time_function(zy.measure_wamr_runtime_cold))
                # WAMR Runtime hot: from cache
                results["measure_wamr_runtime_hot"].append(
                    self.time_function(zy.measure_wamr_runtime_hot))

                # WASM Module cold: mount input channel, re-measure WASM bytecode
                results["measure_wasm_module_cold"].append(
                    self.time_function(lambda: tr.measure_wasm_module_cold(module_id)))
                # WASM Module hot: from cached wasm_module_measurements
                results["measure_wasm_module_hot"].append(
                    self.time_function(lambda: tr.measure_wasm_module_hot(module_id)))

                # Function execution measurements for different input/output sizes
                for size in fn_in_out_sizes:
                    fn_input = os.urandom(size)
                    fn_output = os.urandom(size)
                    results["measure_function"][size].append(
                        self.time_function(
                            lambda s=size, fi=fn_input, fo=fn_output: tr.measure_function(
                                module_id, fi, len(fi), fo, len(fo))
                        )
                    )

            # Shutdown: send shutdown signal to clean up WASM runtime (once)
            try:
                shutdown_data = pack_shutdown_signal()
                tr.invoke_trustlet_bin(shutdown_data, OUTPUT_SIZE)
            except Exception as e:
                print(f"  Shutdown: {e}")

        print()
        print("=" * 70)
        print("Results Summary")
        print("=" * 70)

        # Print summary
        for key in results.keys():
            if key != "measure_function":
                if results[key]:
                    avg = statistics.mean(results[key])
                    med = statistics.median(results[key])
                    stddev = statistics.stdev(results[key]) if len(results[key]) > 1 else 0.0
                    print(f"  {key}: avg={avg:.0f}ns, median={med:.0f}ns, stddev={stddev:.0f}ns")
            else:
                for size, values in results[key].items():
                    if values:
                        avg = statistics.mean(values)
                        med = statistics.median(values)
                        stddev = statistics.stdev(values) if len(values) > 1 else 0.0
                        print(f"  {key} (size={size}): avg={avg:.0f}ns, median={med:.0f}ns, stddev={stddev:.0f}ns")

        # Calculate stats and save to CSV
        self.save_results_to_csv(results)
        print()
        print("Done!")


if __name__ == "__main__":
    import fire
    fire.Fire(Runner)
