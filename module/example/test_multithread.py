#!/usr/bin/env python3
"""
test_multithread.py - Multi-threaded async WASM execution test suite

Tests the new async API: load_module, submit_task, get_result, poll_result,
destroy_runtime.  Designed to run inside the guest VM after deploying the
latest wamr_pal.elf and add.wasm.

Usage (inside the guest VM):
  cd /root/module/example
  python3 test_multithread.py
"""

import sys
import os
import struct
import time

try:
    import wallet
except ImportError:
    print("ERROR: 'wallet' module not found.")
    print("  cd /root/module/python && python3 setup.py install")
    sys.exit(1)

WAMR_PAL_ELF = "./wamr_pal.elf"
DUMMY_MANIFEST = "./dummy.manifest"
DUMMY_LIBOS = "./dummy.libos"
WASM_FILE = "./add.wasm"
DUMMY_FUNCTION = "./dummy_function.txt"

OUTPUT_SIZE = 4096
POLL_TIMEOUT = 30.0
POLL_INTERVAL = 0.05


class TestRunner:
    def __init__(self):
        self.passed = 0
        self.failed = 0
        self.skipped = 0
        self.results = []

    def ok(self, name, msg=""):
        self.passed += 1
        tag = f" ({msg})" if msg else ""
        print(f"  [PASS] {name}{tag}")
        self.results.append(("PASS", name))

    def fail(self, name, msg=""):
        self.failed += 1
        tag = f" ({msg})" if msg else ""
        print(f"  [FAIL] {name}{tag}")
        self.results.append(("FAIL", name))

    def skip(self, name, msg=""):
        self.skipped += 1
        tag = f" ({msg})" if msg else ""
        print(f"  [SKIP] {name}{tag}")
        self.results.append(("SKIP", name))

    def summary(self):
        total = self.passed + self.failed + self.skipped
        print()
        print("=" * 60)
        print(f"Results: {self.passed} passed, {self.failed} failed, "
              f"{self.skipped} skipped / {total} total")
        if self.failed > 0:
            print("FAILED tests:")
            for status, name in self.results:
                if status == "FAIL":
                    print(f"  - {name}")
        print("=" * 60)
        return self.failed == 0


runner = TestRunner()


def check_prerequisites():
    for path, desc in [
        (WAMR_PAL_ELF, "WAMR PAL ELF"),
        (DUMMY_MANIFEST, "Dummy manifest"),
        (DUMMY_LIBOS, "Dummy libos"),
        (WASM_FILE, "WASM module"),
    ]:
        if not os.path.exists(path):
            print(f"ERROR: {desc} not found: {path}")
            sys.exit(1)
    if not os.path.exists(DUMMY_FUNCTION):
        with open(DUMMY_FUNCTION, "w") as f:
            f.write("dummy")


def create_trustlet_env(w):
    """Create a fresh zygote + trustlet pair. Returns (zygote, trustlet)."""
    zygote = w.create_zygote(WAMR_PAL_ELF, DUMMY_MANIFEST, DUMMY_LIBOS)
    trustlet = zygote.create_trustlet(DUMMY_FUNCTION)
    return zygote, trustlet


def load_wasm():
    with open(WASM_FILE, "rb") as f:
        return f.read()


# ---------------------------------------------------------------------------
# Regression: existing sync API (command=0) via invoke_trustlet_bin
# ---------------------------------------------------------------------------

def pack_sync_load_and_invoke(wasm_bytes, func_name, argv):
    func_bytes = func_name.encode("ascii")
    name_padded = (len(func_bytes) + 3) & ~3
    header = struct.pack("<IIHH", len(wasm_bytes), len(func_bytes), len(argv), 0)
    name_data = func_bytes + b"\x00" * (name_padded - len(func_bytes))
    argv_data = b"".join(struct.pack("<I", a) for a in argv)
    return header + name_data + argv_data + wasm_bytes


def pack_sync_invoke_only(func_name, argv):
    func_bytes = func_name.encode("ascii")
    name_padded = (len(func_bytes) + 3) & ~3
    header = struct.pack("<IIHH", 0, len(func_bytes), len(argv), 0)
    name_data = func_bytes + b"\x00" * (name_padded - len(func_bytes))
    argv_data = b"".join(struct.pack("<I", a) for a in argv)
    return header + name_data + argv_data


def parse_output(data):
    if len(data) < 8:
        return (255, 0)
    return struct.unpack("<II", data[:8])


def test_T00_regression(w):
    """T00: Backward-compatible sync invoke via existing API."""
    print("\n--- T00: Regression (sync invoke) ---")
    wasm = load_wasm()
    _, trustlet = create_trustlet_env(w)
    tid = trustlet.process_id

    cases = [
        ("add(3,5)", pack_sync_load_and_invoke(wasm, "add", [3, 5]), 8),
        ("add(10,20) reuse", pack_sync_invoke_only("add", [10, 20]), 30),
        ("multiply(4,7)", pack_sync_invoke_only("multiply", [4, 7]), 28),
        ("get_answer()", pack_sync_invoke_only("get_answer", []), 42),
    ]

    for desc, payload, expected in cases:
        out = trustlet.invoke_trustlet_bin(payload, OUTPUT_SIZE)
        status, result = parse_output(out)
        if status == 0 and result == expected:
            runner.ok(f"T00/{desc}", f"={result}")
        else:
            runner.fail(f"T00/{desc}", f"status={status} result={result} expected={expected}")

    shutdown = struct.pack("<IIHH", 0, 0, 0, 0)
    try:
        trustlet.invoke_trustlet_bin(shutdown, OUTPUT_SIZE)
    except Exception:
        pass


# ---------------------------------------------------------------------------
# Async basics: load_module + submit_task + poll_result
# ---------------------------------------------------------------------------

def test_T01_load_module(trustlet, wasm):
    """T01: load_module returns a valid module_id."""
    print("\n--- T01: load_module ---")
    try:
        mid = trustlet.load_module(wasm)
        if mid >= 0:
            runner.ok("T01/load_module", f"module_id={mid}")
        else:
            runner.fail("T01/load_module", f"bad module_id={mid}")
        return mid
    except Exception as e:
        runner.fail("T01/load_module", str(e))
        return None


def test_T02_single_async(trustlet, mid):
    """T02: Single async task submit + poll."""
    print("\n--- T02: Single async task ---")
    if mid is None:
        runner.skip("T02/single_async", "no module")
        return
    try:
        rid = trustlet.submit_task(mid, "add", [7, 8])
        val = trustlet.poll_result(rid, timeout=POLL_TIMEOUT, interval=POLL_INTERVAL)
        if val == 15:
            runner.ok("T02/add(7,8)", f"={val}")
        else:
            runner.fail("T02/add(7,8)", f"got {val}, expected 15")
    except Exception as e:
        runner.fail("T02/add(7,8)", str(e))


def test_T03_sequential_tasks(trustlet, mid):
    """T03: Multiple tasks submitted and polled sequentially."""
    print("\n--- T03: Sequential async tasks ---")
    if mid is None:
        runner.skip("T03/sequential", "no module")
        return
    cases = [
        ("add(1,2)", "add", [1, 2], 3),
        ("add(100,200)", "add", [100, 200], 300),
        ("multiply(6,9)", "multiply", [6, 9], 54),
        ("get_answer()", "get_answer", [], 42),
    ]
    for desc, func, args, expected in cases:
        try:
            rid = trustlet.submit_task(mid, func, args)
            val = trustlet.poll_result(rid, timeout=POLL_TIMEOUT, interval=POLL_INTERVAL)
            if val == expected:
                runner.ok(f"T03/{desc}", f"={val}")
            else:
                runner.fail(f"T03/{desc}", f"got {val}, expected {expected}")
        except Exception as e:
            runner.fail(f"T03/{desc}", str(e))


# ---------------------------------------------------------------------------
# Concurrency
# ---------------------------------------------------------------------------

def test_T04_two_concurrent(trustlet, mid):
    """T04: Submit 2 tasks, then poll both."""
    print("\n--- T04: Two concurrent tasks ---")
    if mid is None:
        runner.skip("T04/concurrent2", "no module")
        return
    try:
        rid1 = trustlet.submit_task(mid, "add", [10, 20])
        rid2 = trustlet.submit_task(mid, "add", [30, 40])
        v1 = trustlet.poll_result(rid1, timeout=POLL_TIMEOUT, interval=POLL_INTERVAL)
        v2 = trustlet.poll_result(rid2, timeout=POLL_TIMEOUT, interval=POLL_INTERVAL)
        ok1 = v1 == 30
        ok2 = v2 == 70
        if ok1:
            runner.ok("T04/task1 add(10,20)", f"={v1}")
        else:
            runner.fail("T04/task1 add(10,20)", f"got {v1}")
        if ok2:
            runner.ok("T04/task2 add(30,40)", f"={v2}")
        else:
            runner.fail("T04/task2 add(30,40)", f"got {v2}")
    except Exception as e:
        runner.fail("T04/concurrent2", str(e))


def test_T05_max_concurrency(trustlet, mid):
    """T05: Submit tasks up to thread capacity, all should complete."""
    print("\n--- T05: Max concurrency ---")
    if mid is None:
        runner.skip("T05/max_concurrency", "no module")
        return
    n = 4
    try:
        rids = []
        for i in range(n):
            rid = trustlet.submit_task(mid, "add", [i, i * 10])
            rids.append((rid, i + i * 10))
        all_ok = True
        for rid, expected in rids:
            val = trustlet.poll_result(rid, timeout=POLL_TIMEOUT, interval=POLL_INTERVAL)
            if val != expected:
                runner.fail(f"T05/task rid={rid}", f"got {val}, expected {expected}")
                all_ok = False
        if all_ok:
            runner.ok(f"T05/max_concurrency({n} tasks)", "all correct")
    except Exception as e:
        runner.fail("T05/max_concurrency", str(e))


def test_T06_mixed_functions(trustlet, mid):
    """T06: Concurrent tasks calling different functions."""
    print("\n--- T06: Mixed functions concurrent ---")
    if mid is None:
        runner.skip("T06/mixed", "no module")
        return
    try:
        r_add = trustlet.submit_task(mid, "add", [11, 22])
        r_mul = trustlet.submit_task(mid, "multiply", [3, 7])
        r_ans = trustlet.submit_task(mid, "get_answer", [])
        va = trustlet.poll_result(r_add, timeout=POLL_TIMEOUT, interval=POLL_INTERVAL)
        vm = trustlet.poll_result(r_mul, timeout=POLL_TIMEOUT, interval=POLL_INTERVAL)
        vn = trustlet.poll_result(r_ans, timeout=POLL_TIMEOUT, interval=POLL_INTERVAL)
        if va == 33:
            runner.ok("T06/add(11,22)", f"={va}")
        else:
            runner.fail("T06/add(11,22)", f"got {va}")
        if vm == 21:
            runner.ok("T06/multiply(3,7)", f"={vm}")
        else:
            runner.fail("T06/multiply(3,7)", f"got {vm}")
        if vn == 42:
            runner.ok("T06/get_answer()", f"={vn}")
        else:
            runner.fail("T06/get_answer()", f"got {vn}")
    except Exception as e:
        runner.fail("T06/mixed", str(e))


def test_T07_burst(trustlet, mid):
    """T07: Burst submit many tasks, all should eventually complete."""
    print("\n--- T07: Burst submit (8 tasks) ---")
    if mid is None:
        runner.skip("T07/burst", "no module")
        return
    n = 8
    try:
        rids = []
        for i in range(n):
            rid = trustlet.submit_task(mid, "add", [i, 1])
            rids.append((rid, i + 1))
        all_ok = True
        for rid, expected in rids:
            val = trustlet.poll_result(rid, timeout=POLL_TIMEOUT, interval=POLL_INTERVAL)
            if val != expected:
                runner.fail(f"T07/burst rid={rid}", f"got {val}, expected {expected}")
                all_ok = False
        if all_ok:
            runner.ok(f"T07/burst({n} tasks)", "all correct")
    except Exception as e:
        runner.fail("T07/burst", str(e))


# ---------------------------------------------------------------------------
# Polling behavior
# ---------------------------------------------------------------------------

def test_T08_pending(trustlet, mid):
    """T08: get_result may return PENDING for a just-submitted task."""
    print("\n--- T08: PENDING status ---")
    if mid is None:
        runner.skip("T08/pending", "no module")
        return
    try:
        rid = trustlet.submit_task(mid, "add", [99, 1])
        status, value = trustlet.get_result(rid)
        if status == wallet.Trustlet.RESULT_PENDING:
            runner.ok("T08/pending", "got PENDING as expected")
        elif status == 0 and value == 100:
            runner.ok("T08/pending", "task completed instantly (fast execution)")
        else:
            runner.fail("T08/pending", f"unexpected status={status:#x} value={value}")
        val = trustlet.poll_result(rid, timeout=POLL_TIMEOUT, interval=POLL_INTERVAL)
        if val == 100:
            runner.ok("T08/final_result", f"={val}")
        else:
            runner.fail("T08/final_result", f"got {val}, expected 100")
    except Exception as e:
        runner.fail("T08/pending", str(e))


# ---------------------------------------------------------------------------
# Error handling
# ---------------------------------------------------------------------------

def test_T09_invalid_module(trustlet):
    """T09: submit_task with invalid module_id should fail."""
    print("\n--- T09: Invalid module_id ---")
    try:
        rid = trustlet.submit_task(9999, "add", [1, 2])
        runner.fail("T09/invalid_module", f"should have raised, got rid={rid}")
    except RuntimeError as e:
        runner.ok("T09/invalid_module", f"raised: {e}")
    except Exception as e:
        runner.fail("T09/invalid_module", f"unexpected: {e}")


def test_T10_invalid_request(trustlet):
    """T10: get_result with bogus request_id."""
    print("\n--- T10: Invalid request_id ---")
    try:
        status, value = trustlet.get_result(99999)
        if status != 0:
            runner.ok("T10/invalid_request", f"status={status:#x}")
        else:
            runner.fail("T10/invalid_request",
                        f"expected error status, got 0 value={value}")
    except RuntimeError as e:
        runner.ok("T10/invalid_request", f"raised: {e}")
    except Exception as e:
        runner.fail("T10/invalid_request", f"unexpected: {e}")


def test_T11_bad_function_name(trustlet, mid):
    """T11: submit_task with nonexistent function name."""
    print("\n--- T11: Nonexistent function ---")
    if mid is None:
        runner.skip("T11/bad_func", "no module")
        return
    try:
        rid = trustlet.submit_task(mid, "no_such_function", [1])
        val_or_err = trustlet.poll_result(rid, timeout=10.0, interval=POLL_INTERVAL)
        runner.fail("T11/bad_func",
                    f"expected error but got value={val_or_err}")
    except (RuntimeError, TimeoutError) as e:
        runner.ok("T11/bad_func", f"error as expected: {e}")
    except Exception as e:
        runner.fail("T11/bad_func", f"unexpected: {e}")


# ---------------------------------------------------------------------------
# Lifecycle
# ---------------------------------------------------------------------------

def test_T12_destroy(trustlet):
    """T12: destroy_runtime should succeed."""
    print("\n--- T12: Destroy runtime ---")
    try:
        trustlet.destroy_runtime()
        runner.ok("T12/destroy", "clean shutdown")
    except Exception as e:
        runner.fail("T12/destroy", str(e))


def test_T13_second_lifecycle(w, wasm):
    """T13: Create a second trustlet after destroying the first, full cycle."""
    print("\n--- T13: Second lifecycle ---")
    try:
        _, trustlet2 = create_trustlet_env(w)
        mid2 = trustlet2.load_module(wasm)
        rid2 = trustlet2.submit_task(mid2, "add", [50, 50])
        val2 = trustlet2.poll_result(rid2, timeout=POLL_TIMEOUT, interval=POLL_INTERVAL)
        if val2 == 100:
            runner.ok("T13/second_lifecycle add(50,50)", f"={val2}")
        else:
            runner.fail("T13/second_lifecycle", f"got {val2}, expected 100")
        trustlet2.destroy_runtime()
        runner.ok("T13/second_destroy", "clean")
    except Exception as e:
        runner.fail("T13/second_lifecycle", str(e))


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    print("=" * 60)
    print("Multi-threaded Async WASM Execution — Test Suite")
    print("=" * 60)

    check_prerequisites()
    wasm = load_wasm()
    print(f"WASM module: {len(wasm)} bytes")

    with wallet.Wallet() as w:
        # --- Regression ---
        test_T00_regression(w)

        # --- Async tests on a fresh trustlet ---
        print("\n" + "=" * 60)
        print("Async API Tests")
        print("=" * 60)
        _, trustlet = create_trustlet_env(w)
        print(f"Trustlet ID: {trustlet.process_id}")

        mid = test_T01_load_module(trustlet, wasm)
        test_T02_single_async(trustlet, mid)
        test_T03_sequential_tasks(trustlet, mid)
        test_T04_two_concurrent(trustlet, mid)
        test_T05_max_concurrency(trustlet, mid)
        test_T06_mixed_functions(trustlet, mid)
        test_T07_burst(trustlet, mid)
        test_T08_pending(trustlet, mid)

        # --- Error handling ---
        print("\n" + "=" * 60)
        print("Error Handling Tests")
        print("=" * 60)
        test_T09_invalid_module(trustlet)
        test_T10_invalid_request(trustlet)
        test_T11_bad_function_name(trustlet, mid)

        # --- Lifecycle ---
        print("\n" + "=" * 60)
        print("Lifecycle Tests")
        print("=" * 60)
        test_T12_destroy(trustlet)
        test_T13_second_lifecycle(w, wasm)

    success = runner.summary()
    sys.exit(0 if success else 1)


if __name__ == "__main__":
    main()
