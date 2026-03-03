#!/usr/bin/env python3
"""
test_wamr_async.py - Async multi-threaded WASM execution tests

Tests the multi-vCPU architecture where each worker thread runs on a
dedicated vCPU.  Uses the new async APIs: load_module, submit_task,
get_result (non-blocking), poll_result (blocking), destroy_runtime.

Usage (inside guest VM):
  cd /root/module/example && python3 test_wamr_async.py
"""

import sys
import os
import time

try:
    import wallet
except ImportError:
    print("ERROR: 'wallet' module not found. Install it first:")
    print("  cd /root/module/python && python3 setup.py install")
    sys.exit(1)

WAMR_PAL_ELF = "./wamr_pal.elf"
DUMMY_MANIFEST = "./dummy.manifest"
DUMMY_LIBOS = "./dummy.libos"
DUMMY_FUNCTION = "./dummy_function.txt"
ADD_WASM = "./add.wasm"
LOOP_WASM = "./loop.wasm"
OUTPUT_SIZE = 4096
POLL_TIMEOUT = 30.0
POLL_INTERVAL = 0.05


class TestRunner:
    def __init__(self):
        self.pass_count = 0
        self.fail_count = 0
        self.w = None
        self.zygote = None
        self.trustlet = None

    def setup(self):
        if not os.path.exists(DUMMY_FUNCTION):
            with open(DUMMY_FUNCTION, "w") as f:
                f.write("dummy")
        for path in (WAMR_PAL_ELF, DUMMY_MANIFEST, DUMMY_LIBOS, ADD_WASM):
            if not os.path.exists(path):
                print(f"FATAL: missing {path}")
                sys.exit(1)

        self.w = wallet.Wallet()
        self.w.open_device()
        self._create_trustlet()

    def _create_trustlet(self):
        self.zygote = self.w.create_zygote(
            WAMR_PAL_ELF, DUMMY_MANIFEST, DUMMY_LIBOS)
        self.trustlet = self.zygote.create_trustlet(DUMMY_FUNCTION)

    def teardown(self):
        if self.w:
            self.w.close_device()

    def ok(self, name, cond, detail=""):
        if cond:
            self.pass_count += 1
            print(f"  PASS: {name}")
        else:
            self.fail_count += 1
            print(f"  FAIL: {name}  {detail}")

    # ------------------------------------------------------------------
    # Group 1: Async basics
    # ------------------------------------------------------------------

    def test_load_module(self):
        """T01: load_module returns a valid module_id."""
        with open(ADD_WASM, "rb") as f:
            wasm = f.read()
        mid = self.trustlet.load_module(wasm)
        self.ok("T01 load_module", mid >= 0, f"module_id={mid}")
        return mid

    def test_single_async(self, mid):
        """T02: single async add(3,5)=8."""
        rid = self.trustlet.submit_task(mid, "add", [3, 5])
        val = self.trustlet.poll_result(rid, POLL_TIMEOUT, POLL_INTERVAL)
        self.ok("T02 add(3,5)=8", val == 8, f"got {val}")

    def test_multi_call_same_module(self, mid):
        """T03: multiple calls on the same module."""
        rid1 = self.trustlet.submit_task(mid, "add", [10, 20])
        v1 = self.trustlet.poll_result(rid1, POLL_TIMEOUT, POLL_INTERVAL)
        self.ok("T03a add(10,20)=30", v1 == 30, f"got {v1}")

        rid2 = self.trustlet.submit_task(mid, "multiply", [4, 7])
        v2 = self.trustlet.poll_result(rid2, POLL_TIMEOUT, POLL_INTERVAL)
        self.ok("T03b multiply(4,7)=28", v2 == 28, f"got {v2}")

    # ------------------------------------------------------------------
    # Group 2: Concurrent execution
    # ------------------------------------------------------------------

    def test_dual_concurrent(self, mid):
        """T04: submit two tasks before polling either."""
        r1 = self.trustlet.submit_task(mid, "add", [1, 2])
        r2 = self.trustlet.submit_task(mid, "add", [3, 4])
        v1 = self.trustlet.poll_result(r1, POLL_TIMEOUT, POLL_INTERVAL)
        v2 = self.trustlet.poll_result(r2, POLL_TIMEOUT, POLL_INTERVAL)
        self.ok("T04a add(1,2)=3", v1 == 3, f"got {v1}")
        self.ok("T04b add(3,4)=7", v2 == 7, f"got {v2}")

    def test_different_funcs_concurrent(self, mid):
        """T05: concurrent tasks calling different functions."""
        r1 = self.trustlet.submit_task(mid, "add", [5, 6])
        r2 = self.trustlet.submit_task(mid, "multiply", [3, 3])
        v1 = self.trustlet.poll_result(r1, POLL_TIMEOUT, POLL_INTERVAL)
        v2 = self.trustlet.poll_result(r2, POLL_TIMEOUT, POLL_INTERVAL)
        self.ok("T05a add(5,6)=11", v1 == 11, f"got {v1}")
        self.ok("T05b multiply(3,3)=9", v2 == 9, f"got {v2}")

    def test_compute_intensive(self):
        """T06: concurrent fib calls using loop.wasm."""
        if not os.path.exists(LOOP_WASM):
            print("  SKIP: T06 (loop.wasm not found)")
            return
        with open(LOOP_WASM, "rb") as f:
            wasm = f.read()
        mid = self.trustlet.load_module(wasm)
        r1 = self.trustlet.submit_task(mid, "fib", [30])
        r2 = self.trustlet.submit_task(mid, "fib", [25])
        v1 = self.trustlet.poll_result(r1, POLL_TIMEOUT, POLL_INTERVAL)
        v2 = self.trustlet.poll_result(r2, POLL_TIMEOUT, POLL_INTERVAL)
        self.ok("T06a fib(30)=832040", v1 == 832040, f"got {v1}")
        self.ok("T06b fib(25)=75025", v2 == 75025, f"got {v2}")

    def test_batch_submit(self, mid):
        """T07: submit 5 tasks then collect all results."""
        pairs = [(1, 2), (10, 20), (100, 200), (7, 8), (0, 0)]
        rids = [self.trustlet.submit_task(mid, "add", list(p)) for p in pairs]
        results = [self.trustlet.poll_result(r, POLL_TIMEOUT, POLL_INTERVAL)
                   for r in rids]
        expected = [a + b for a, b in pairs]
        all_ok = results == expected
        self.ok("T07 batch(5 adds)", all_ok,
                f"expected {expected}, got {results}")

    # ------------------------------------------------------------------
    # Group 3: Polling
    # ------------------------------------------------------------------

    def test_pending_poll(self):
        """T08: immediate get_result may return PENDING, then poll succeeds."""
        if not os.path.exists(LOOP_WASM):
            print("  SKIP: T08 (loop.wasm not found)")
            return
        with open(LOOP_WASM, "rb") as f:
            wasm = f.read()
        mid = self.trustlet.load_module(wasm)
        rid = self.trustlet.submit_task(mid, "fib", [30])
        status, _ = self.trustlet.get_result(rid)
        saw_pending = (status == wallet.Trustlet.RESULT_PENDING)
        val = self.trustlet.poll_result(rid, POLL_TIMEOUT, POLL_INTERVAL)
        self.ok("T08 fib(30) poll", val == 832040, f"got {val}")
        if saw_pending:
            print("    (saw PENDING before completion)")

    def test_delayed_get(self, mid):
        """T09: submit, sleep 2s, then get result (should be done)."""
        rid = self.trustlet.submit_task(mid, "add", [42, 58])
        time.sleep(2)
        status, value = self.trustlet.get_result(rid)
        self.ok("T09 delayed get", status == 0 and value == 100,
                f"status={status}, value={value}")

    # ------------------------------------------------------------------
    # Group 4: Error handling
    # ------------------------------------------------------------------

    def test_invalid_module_id(self):
        """T10: submit_task with bad module_id."""
        try:
            self.trustlet.submit_task(999, "add", [1, 2])
            self.ok("T10 invalid module_id", False, "no exception raised")
        except RuntimeError:
            self.ok("T10 invalid module_id", True)

    def test_invalid_func_name(self, mid):
        """T11: submit_task with nonexistent function."""
        try:
            rid = self.trustlet.submit_task(mid, "nonexistent", [])
            val = self.trustlet.poll_result(rid, 5, POLL_INTERVAL)
            self.ok("T11 invalid func", False,
                    f"expected error, got val={val}")
        except (RuntimeError, TimeoutError):
            self.ok("T11 invalid func", True)

    def test_invalid_request_id(self):
        """T12: get_result with bad request_id."""
        status, _ = self.trustlet.get_result(99999)
        self.ok("T12 invalid request_id", status != 0,
                f"status={status}")

    # ------------------------------------------------------------------
    # Group 5: Lifecycle
    # ------------------------------------------------------------------

    def test_destroy_and_rebuild(self):
        """T13: destroy runtime, rebuild trustlet, run again."""
        with open(ADD_WASM, "rb") as f:
            wasm = f.read()
        mid = self.trustlet.load_module(wasm)
        rid = self.trustlet.submit_task(mid, "add", [100, 200])
        v = self.trustlet.poll_result(rid, POLL_TIMEOUT, POLL_INTERVAL)
        self.ok("T13a round1 add(100,200)=300", v == 300, f"got {v}")

        self.trustlet.destroy_runtime()
        self._create_trustlet()

        mid2 = self.trustlet.load_module(wasm)
        rid2 = self.trustlet.submit_task(mid2, "add", [1000, 2000])
        v2 = self.trustlet.poll_result(rid2, POLL_TIMEOUT, POLL_INTERVAL)
        self.ok("T13b round2 add(1000,2000)=3000", v2 == 3000, f"got {v2}")

    def test_regression_sync(self):
        """T14: after async tests, old sync invoke still works."""
        import struct
        with open(ADD_WASM, "rb") as f:
            wasm = f.read()
        func_name = b"add"
        name_padded = func_name + b"\x00"
        argv = struct.pack("<II", 7, 8)
        header = struct.pack("<IIHH", len(wasm), len(func_name), 2, 0)
        payload = header + name_padded + argv + wasm
        result_bytes = self.trustlet.invoke_trustlet_bin(payload, OUTPUT_SIZE)
        if len(result_bytes) >= 8:
            status, result = struct.unpack("<II", result_bytes[:8])
            self.ok("T14 sync add(7,8)=15",
                    status == 0 and result == 15,
                    f"status={status}, result={result}")
        else:
            self.ok("T14 sync invoke", False, "short output")

    # ------------------------------------------------------------------

    def run_all(self):
        print("=" * 60)
        print("Async Multi-Thread WASM Test Suite")
        print("=" * 60)
        print()

        self.setup()

        print("[Group 1] Async basics")
        mid = self.test_load_module()
        self.test_single_async(mid)
        self.test_multi_call_same_module(mid)
        print()

        print("[Group 2] Concurrent execution")
        self.test_dual_concurrent(mid)
        self.test_different_funcs_concurrent(mid)
        self.test_compute_intensive()
        self.test_batch_submit(mid)
        print()

        print("[Group 3] Polling")
        self.test_pending_poll()
        self.test_delayed_get(mid)
        print()

        print("[Group 4] Error handling")
        self.test_invalid_module_id()
        self.test_invalid_func_name(mid)
        self.test_invalid_request_id()
        print()

        print("[Group 5] Lifecycle")
        self.test_destroy_and_rebuild()
        self.test_regression_sync()
        print()

        self.teardown()

        total = self.pass_count + self.fail_count
        print("=" * 60)
        print(f"Results: {self.pass_count}/{total} passed, "
              f"{self.fail_count} failed")
        if self.fail_count == 0:
            print("ALL TESTS PASSED!")
        else:
            print("SOME TESTS FAILED")
        print("=" * 60)
        return self.fail_count == 0


if __name__ == "__main__":
    runner = TestRunner()
    success = runner.run_all()
    sys.exit(0 if success else 1)
