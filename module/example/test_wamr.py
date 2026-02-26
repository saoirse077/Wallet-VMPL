#!/usr/bin/env python3
"""
test_wamr.py - Phase 3 test for WAMR PAL bare-metal ELF in VMPL1

This script demonstrates the complete Wallet-VMPL execution flow with WAMR:

  1. create_zygote(wamr_pal.elf) → SVSM loads ELF, runs early_invoke
     → VMPL1 initializes heap + WAMR runtime → pal_svsm_exit(0) suspends
  2. zygote.create_trustlet(dummy) → SVSM CoW duplicates the zygote
  3. trustlet.invoke_trustlet_bin(packed_input, output_size)
     → SVSM copies packed_input to input channel → resumes VMPL1
     → VMPL1 reads input → loads WASM → invokes function → writes output
     → pal_svsm_get_result() → SVSM copy_out to Guest buffer
     → Python receives result bytes
  4. Parse result bytes → print result in Guest terminal

Input channel protocol (packed by this script):
  [0..3]   uint32_t wasm_size      // WASM bytecode size
  [4..7]   uint32_t func_name_len  // Function name length (without \\0)
  [8..9]   uint16_t argc           // Number of i32 arguments
  [10..11] uint16_t reserved       // Padding (0)
  [12..12+func_name_len-1] func_name  // Function name bytes
  [aligned to 4 bytes]
  [arg_offset..] uint32_t argv[]   // Function arguments (i32 each)
  [wasm_offset..] uint8_t wasm[]   // WASM bytecode

Output channel protocol (returned by invoke_trustlet_bin):
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
DUMMY_FUNCTION = "./dummy_function.txt"


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

    # Create dummy_function.txt if it doesn't exist
    # (needed by create_trustlet which reads a file)
    if not os.path.exists(DUMMY_FUNCTION):
        with open(DUMMY_FUNCTION, "w") as f:
            f.write("dummy")

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


def pack_input(wasm_bytes, func_name, argv):
    """
    Pack the input channel data according to our protocol.

    Returns bytes to be passed to invoke_trustlet_bin.
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


def parse_output(output_bytes):
    """
    Parse the output channel data returned by invoke_trustlet_bin.

    Returns (status, result) tuple.
    """
    if len(output_bytes) < 8:
        return (255, 0)  # Error: insufficient data
    status, result = struct.unpack("<II", output_bytes[:8])
    return (status, result)


def main():
    print("=" * 60)
    print("WAMR-PAL Phase 3 Test — Complete Wallet-VMPL Execution Flow")
    print("=" * 60)
    print()

    # ---- Step 1: Check files ----
    print("[1/5] Checking required files...")
    check_files()
    print()

    # ---- Step 2: Read WASM module ----
    print("[2/5] Reading WASM module...")
    with open(WASM_FILE, "rb") as f:
        wasm_bytes = f.read()
    print(f"  Loaded {len(wasm_bytes)} bytes from {WASM_FILE}")
    print()

    # ---- Step 3: Create Zygote ----
    print("[3/5] Creating Zygote (initializes WAMR runtime in VMPL1)...")
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

        # ---- Step 4: Create Trustlet (CoW duplicate) ----
        print("[4/5] Creating Trustlet (CoW duplicate of Zygote)...")
        try:
            trustlet = zygote.create_trustlet(DUMMY_FUNCTION)
            trustlet_id = trustlet.process_id
            print(f"  Trustlet created with ID: {trustlet_id}")
        except Exception as e:
            print(f"  FAILED: {e}")
            sys.exit(1)
        print()

        # ---- Step 5: Invoke WASM function ----
        print("[5/5] Invoking WASM functions via invoke_trustlet_bin...")
        print()

        # Test 1: add(3, 5) → expected 8
        print("  --- Test 1: add(3, 5) ---")
        input_data = pack_input(wasm_bytes, "add", [3, 5])
        print(f"  Input payload: {len(input_data)} bytes")
        print(f"    Header: wasm_size={len(wasm_bytes)}, func='add', argc=2, argv=[3, 5]")

        OUTPUT_SIZE = 4096  # Must be >= 8 bytes for our protocol
        try:
            result_bytes = trustlet.invoke_trustlet_bin(input_data, OUTPUT_SIZE)
            status, result = parse_output(result_bytes)
            print(f"  Output: status={status}, result={result}")
            if status == 0 and result == 8:
                print("  *** PASS: add(3, 5) == 8 ***")
            else:
                print(f"  *** FAIL: expected status=0, result=8, got status={status}, result={result} ***")
        except Exception as e:
            print(f"  FAILED: {e}")
        print()

        # Test 2: multiply(4, 7) → expected 28
        print("  --- Test 2: multiply(4, 7) ---")
        input_data = pack_input(wasm_bytes, "multiply", [4, 7])
        print(f"  Input payload: {len(input_data)} bytes")

        try:
            result_bytes = trustlet.invoke_trustlet_bin(input_data, OUTPUT_SIZE)
            status, result = parse_output(result_bytes)
            print(f"  Output: status={status}, result={result}")
            if status == 0 and result == 28:
                print("  *** PASS: multiply(4, 7) == 28 ***")
            else:
                print(f"  *** FAIL: expected status=0, result=28, got status={status}, result={result} ***")
        except Exception as e:
            print(f"  FAILED: {e}")
        print()

        # Test 3: get_answer() → expected 42
        print("  --- Test 3: get_answer() ---")
        input_data = pack_input(wasm_bytes, "get_answer", [])
        print(f"  Input payload: {len(input_data)} bytes")

        try:
            result_bytes = trustlet.invoke_trustlet_bin(input_data, OUTPUT_SIZE)
            status, result = parse_output(result_bytes)
            print(f"  Output: status={status}, result={result}")
            if status == 0 and result == 42:
                print("  *** PASS: get_answer() == 42 ***")
            else:
                print(f"  *** FAIL: expected status=0, result=42, got status={status}, result={result} ***")
        except Exception as e:
            print(f"  FAILED: {e}")
        print()

        # Cleanup
        # NOTE: Deliberately NOT calling delete() here.
        # The original Wallet test.py also does not call delete().
        # Calling delete() triggers Zygote/Trustlet Drop in SVSM which
        # has incomplete resource cleanup (VMSA page, MPK_MANAGER state,
        # bump allocator address space not reclaimed), causing the second
        # run of this script to hang during create_zygote.
        #
        # Without delete(), each run consumes 2 PROCESS_STORE slots
        # (max 64 slots = 32 runs before exhaustion). This is acceptable
        # for development/testing. A proper fix requires SVSM-side changes.
        print("Skipping cleanup (no delete) — matches original Wallet test.py behavior")

    print()
    print("=" * 60)
    print("Phase 3 test completed.")
    print()
    print("Results are displayed above in the Guest terminal.")
    print("Also check SVSM serial console for [WAMR-PAL] debug output.")
    print("=" * 60)


if __name__ == "__main__":
    main()
