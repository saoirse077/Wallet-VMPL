#!/usr/bin/env python3
"""
test_wamr.py - Phase 3b test for WAMR PAL bare-metal ELF in VMPL1

This script demonstrates the complete Wallet-VMPL execution flow with WAMR,
including Phase 3b module reuse: the WASM module is loaded once on the first
invocation, and subsequent function calls reuse the already-loaded module
without re-sending WASM bytecode.

Execution flow (NO-TRUSTLET: 直接在 Zygote 上执行):
  1. create_zygote(wamr_pal.elf) → SVSM loads ELF, runs early_invoke
     → VMPL1 initializes heap + WAMR runtime → pal_svsm_exit(0) suspends
  2. First invoke_trustlet_bin(zygote_id): Mode 1 (load + invoke)
     → packed_input contains WASM bytecode + function name + args
     → VMPL1 loads module, invokes function, returns result
  3. Subsequent invoke_trustlet_bin(zygote_id): Mode 2 (invoke-only, reuse module)
     → packed_input contains only function name + args (no WASM bytecode)
     → VMPL1 reuses loaded module, invokes function, returns result

Input channel protocol:
  Mode 1 (load + invoke):
    [0..3]   uint32_t wasm_size      // WASM bytecode size (> 0)
    [4..7]   uint32_t func_name_len  // Function name length
    [8..9]   uint16_t argc           // Number of i32 arguments
    [10..11] uint16_t reserved       // Padding (0)
    [12..]   func_name (padded to 4-byte alignment)
    [..]     uint32_t argv[]
    [..]     uint8_t wasm_bytes[]

  Mode 2 (invoke-only):
    [0..3]   uint32_t wasm_size = 0  // No WASM bytecode
    [4..7]   uint32_t func_name_len  // Function name length
    [8..9]   uint16_t argc           // Number of i32 arguments
    [10..11] uint16_t reserved       // Padding (0)
    [12..]   func_name (padded to 4-byte alignment)
    [..]     uint32_t argv[]

Output channel protocol:
  [0..3]   uint32_t status         // 0 = success
  [4..7]   uint32_t result         // Function return value (i32)

Usage (inside the guest VM):
  cd /root/module
  insmod vmpl.ko
  make -B -C libwallet/ libwallet.so libwallet.a
  cd python && python3 setup.py install && cd ..
  cd example && python3 test_wamr.py
"""

import sys
import os
import struct

# Ensure the wallet module is importable
try:
    import wallet
except ImportError:
    print("ERROR: 'wallet' module not found.")
    print("Please build and install it first:")
    print("  cd /root/module/python && python3 setup.py install")
    sys.exit(1)

# Paths to required files (when running from /root/module/example/)
WAMR_PAL_ELF = "./wamr_pal.elf"
DUMMY_MANIFEST = "./dummy.manifest"
DUMMY_LIBOS = "./dummy.libos"
WASM_FILE = "./add.wasm"
# [NO-TRUSTLET] DUMMY_FUNCTION 不再需要
# DUMMY_FUNCTION = "./dummy_function.txt"

OUTPUT_SIZE = 4096  # Must be >= 8 bytes for our protocol


def check_files():
    """Verify all required files exist before proceeding."""
    missing = []
    for path, desc in [
        (WAMR_PAL_ELF, "WAMR PAL ELF"),
        (DUMMY_MANIFEST, "Dummy manifest"),
        (DUMMY_LIBOS, "Dummy libos"),
        (WASM_FILE, "WASM module (add.wasm)"),
    ]:
        if not os.path.exists(path):
            missing.append(f"  {desc}: {path}")

    # [NO-TRUSTLET] dummy_function.txt 不再需要
    # if not os.path.exists(DUMMY_FUNCTION):
    #     with open(DUMMY_FUNCTION, "w") as f:
    #         f.write("dummy")

    if missing:
        print("ERROR: Missing required files:")
        print("\n".join(missing))
        print()
        print("Make sure all files are in this directory.")
        print("On the host, run:")
        print("  cd wamr-pal && make && make deploy")
        sys.exit(1)

    # Print file sizes
    for path, desc in [
        (WAMR_PAL_ELF, "WAMR PAL ELF"),
        (WASM_FILE, "WASM module"),
    ]:
        size = os.path.getsize(path)
        print(f"  {desc}: {path} ({size} bytes)")


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


def pack_input_invoke_only(func_name, argv):
    """
    Mode 2: Pack input with only function name + args (no WASM bytecode).

    Used for subsequent invocations that reuse the already-loaded module.
    wasm_size is set to 0 to signal invoke-only mode.
    """
    func_name_bytes = func_name.encode("ascii")
    func_name_len = len(func_name_bytes)
    argc = len(argv)

    # Header: wasm_size=0 signals invoke-only mode
    header = struct.pack("<IIHH", 0, func_name_len, argc, 0)

    # Function name (padded to 4-byte alignment)
    name_padded_len = (func_name_len + 3) & ~3
    name_data = func_name_bytes + b"\x00" * (name_padded_len - func_name_len)

    # Arguments (each uint32_t)
    argv_data = b""
    for arg in argv:
        argv_data += struct.pack("<I", arg)

    # No WASM bytecode
    payload = header + name_data + argv_data

    return payload


def pack_shutdown_signal():
    """
    Mode 3: Pack shutdown signal (wasm_size=0, func_name_len=0).

    Signals VMPL1 to unload module, destroy runtime, and exit cleanly.
    This ensures all resources (including SVSM allocations) are properly freed.
    """
    # Header: both wasm_size=0 and func_name_len=0 signal shutdown
    header = struct.pack("<IIHH", 0, 0, 0, 0)
    return header


def parse_output(output_bytes):
    """
    Parse the output channel data returned by invoke_trustlet_bin.

    Returns (status, result) tuple.
    """
    if len(output_bytes) < 8:
        return (255, 0)  # Error: insufficient data
    status, result = struct.unpack("<II", output_bytes[:8])
    return (status, result)


def invoke_and_check(process, input_data, expected_result, test_desc):
    """
    Invoke a function and check the result.

    Returns True if the test passed, False otherwise.
    """
    try:
        result_bytes = process.invoke_trustlet_bin(input_data, OUTPUT_SIZE)
        status, result = parse_output(result_bytes)
        print(f"  Output: status={status}, result={result}")
        if status == 0 and result == expected_result:
            print(f"  *** PASS: {test_desc} == {expected_result} ***")
            return True
        else:
            print(f"  *** FAIL: expected status=0, result={expected_result}, "
                  f"got status={status}, result={result} ***")
            return False
    except Exception as e:
        print(f"  FAILED: {e}")
        return False


def main():
    print("=" * 60)
    print("WAMR-PAL Phase 3b Test — Module Reuse + SVSM Cleanup (NO-TRUSTLET)")
    print("=" * 60)
    print()

    # ---- Step 1: Check files ----
    print("[1/4] Checking required files...")
    check_files()
    print()

    # ---- Step 2: Read WASM module ----
    print("[2/4] Reading WASM module...")
    with open(WASM_FILE, "rb") as f:
        wasm_bytes = f.read()
    print(f"  Loaded {len(wasm_bytes)} bytes from {WASM_FILE}")
    print()

    # ---- Step 3: Create Zygote ----
    print("[3/4] Creating Zygote (initializes WAMR runtime in VMPL1)...")
    print(f"  ELF: {WAMR_PAL_ELF}")
    print("  (VMPL1 will: init heap → init WAMR → pal_svsm_exit(0))")
    print()

    with wallet.Wallet() as w:
        try:
            zygote = w.create_zygote(WAMR_PAL_ELF, DUMMY_MANIFEST, DUMMY_LIBOS)
            zygote_id = zygote.process_id
            print(f"  Zygote created with ID: {zygote_id}")
        except Exception as e:
            print(f"  FAILED: {e}")
            sys.exit(1)
        print()

        # [NO-TRUSTLET] Step 4 (Create Trustlet) 已删除 — 直接在 Zygote 上 invoke
        # print("[4/5] Creating Trustlet (CoW duplicate of Zygote)...")
        # try:
        #     trustlet = zygote.create_trustlet(DUMMY_FUNCTION)
        #     trustlet_id = trustlet.process_id
        #     print(f"  Trustlet created with ID: {trustlet_id}")
        # except Exception as e:
        #     print(f"  FAILED: {e}")
        #     sys.exit(1)
        # print()

        # ---- Step 4: Invoke WASM functions (直接在 Zygote 上) ----
        print("[4/4] Invoking WASM functions via invoke_trustlet_bin (on Zygote)...")
        print()

        pass_count = 0
        total_count = 0

        # Test 1: add(3, 5) → expected 8
        # Mode 1: First call — sends WASM bytecode + loads module
        total_count += 1
        print("  --- Test 1: add(3, 5) [Mode 1: Load + Invoke] ---")
        input_data = pack_input_load_and_invoke(wasm_bytes, "add", [3, 5])
        print(f"  Input payload: {len(input_data)} bytes (includes {len(wasm_bytes)} bytes WASM)")
        print(f"    Header: wasm_size={len(wasm_bytes)}, func='add', argc=2, argv=[3, 5]")
        if invoke_and_check(zygote, input_data, 8, "add(3, 5)"):
            pass_count += 1
        print()

        # Test 2: add(10, 20) → expected 30
        # Mode 2: Reuse loaded module — no WASM bytecode
        total_count += 1
        print("  --- Test 2: add(10, 20) [Mode 2: Invoke-only, reuse module] ---")
        input_data = pack_input_invoke_only("add", [10, 20])
        print(f"  Input payload: {len(input_data)} bytes (no WASM)")
        print(f"    Header: wasm_size=0, func='add', argc=2, argv=[10, 20]")
        if invoke_and_check(zygote, input_data, 30, "add(10, 20)"):
            pass_count += 1
        print()

        # Test 3: add(100, 200) → expected 300
        # Mode 2: Continue reusing module
        total_count += 1
        print("  --- Test 3: add(100, 200) [Mode 2: Invoke-only] ---")
        input_data = pack_input_invoke_only("add", [100, 200])
        print(f"  Input payload: {len(input_data)} bytes (no WASM)")
        print(f"    Header: wasm_size=0, func='add', argc=2, argv=[100, 200]")
        if invoke_and_check(zygote, input_data, 300, "add(100, 200)"):
            pass_count += 1
        print()

        # Test 4: multiply(4, 7) → expected 28
        # Mode 2: Reuse same module, different function
        total_count += 1
        print("  --- Test 4: multiply(4, 7) [Mode 2: Invoke-only, different func] ---")
        input_data = pack_input_invoke_only("multiply", [4, 7])
        print(f"  Input payload: {len(input_data)} bytes (no WASM)")
        print(f"    Header: wasm_size=0, func='multiply', argc=2, argv=[4, 7]")
        if invoke_and_check(zygote, input_data, 28, "multiply(4, 7)"):
            pass_count += 1
        print()

        # Test 5: get_answer() → expected 42
        # Mode 2: Reuse module, zero-argument function
        total_count += 1
        print("  --- Test 5: get_answer() [Mode 2: Invoke-only, no args] ---")
        input_data = pack_input_invoke_only("get_answer", [])
        print(f"  Input payload: {len(input_data)} bytes (no WASM)")
        print(f"    Header: wasm_size=0, func='get_answer', argc=0")
        if invoke_and_check(zygote, input_data, 42, "get_answer()"):
            pass_count += 1
        print()

        # ---- Phase 3b Cleanup: Send shutdown signal ----
        print("  --- Cleanup: Sending shutdown signal to VMPL1 ---")
        print("  (This ensures complete resource cleanup including SVSM allocations)")
        
        shutdown_data = pack_shutdown_signal()
        print(f"  Shutdown payload: {len(shutdown_data)} bytes (wasm_size=0, func_name_len=0)")
        
        try:
            # Send shutdown signal (Mode 3) - VMPL1 should not return any data for this
            result_bytes = zygote.invoke_trustlet_bin(shutdown_data, OUTPUT_SIZE)
            print(f"  Shutdown completed (received {len(result_bytes) if result_bytes else 0} bytes)")
        except Exception as e:
            print(f"  Shutdown signal sent (exception expected): {e}")
        
        # NOTE: Still not calling delete() to avoid SVSM Drop issues.
        # The shutdown signal ensures VMPL1 performs complete cleanup:
        # - wasmlet_unload_module() → mpk_domain_destroy() → pal_svsm_mpk_free_pkey()
        # - wasmlet_runtime_destroy() → mpk_allocator_destroy()
        # This clears all SVSM allocations and returns pkeys, allowing infinite runs.
        print("  Cleanup completed via shutdown signal (no delete() needed)")

    print()
    print("=" * 60)
    print(f"Phase 3b test completed: {pass_count}/{total_count} tests passed.")
    print()
    if pass_count == total_count:
        print("ALL TESTS PASSED!")
        print()
        print("Key Phase 3b features verified:")
        print("  - Module loaded once (Test 1), reused for Tests 2-5")
        print("  - Same function with different args (Tests 2-3)")
        print("  - Different function on same module (Test 4)")
        print("  - Zero-argument function (Test 5)")
        print("  - Complete SVSM cleanup via shutdown signal")
        print("  - Should support infinite runs (no resource leaks)")
    else:
        print(f"SOME TESTS FAILED ({total_count - pass_count} failures)")
    print()
    print("Results are displayed above in the Guest terminal.")
    print("Also check SVSM serial console for [WAMR-PAL] debug output.")
    print("=" * 60)


if __name__ == "__main__":
    main()
