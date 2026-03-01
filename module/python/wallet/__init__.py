# -*- coding: utf-8 -*-

# _wallet is implemented in ./src_ext
# technically we can implement everything in python but use C module to
# reuse the existing code; it's also easy to prototype low-level code
import _wallet as _w

import os
from pathlib import Path
from typing import TypeAlias, List, Tuple, Dict, Any

FileName: TypeAlias = str | os.PathLike


class Wallet:
    def __init__(self):
        self.fd = None
        pass

    def stat_get(self):
        _w.stat_get()

    def stat_reset(self):
        _w.stat_reset()

    def open_device(self, path: FileName = "/dev/vmpl_device") -> int:
        self.fd = _w.monitor_connect()
        if self.fd < 0:
            raise Exception(f"Failed to open {path}, forgot to load the kernel module?")
        return self.fd

    def close_device(self) -> None:
        if self.fd:
            _w.monitor_close()
            self.fd = None

    def create_zygote(
        self, zygote: FileName, manifest: FileName, libos: FileName
    ) -> int:
        if not Path(zygote).exists():
            raise Exception(f"Zygote {zygote} not found")
        if not Path(manifest).exists():
            raise Exception(f"Manifest {manifest} not found")
        if not Path(libos).exists():
            raise Exception(f"LibOS {libos} not found")
        zygote_id = _w.create_zygote(zygote, manifest, libos)
        if zygote_id < 0:
            raise Exception(f"Failed to create zygote {zygote}")
        return Zygote(zygote_id)

    def attest_monitor(self) -> str:
        if not self.fd:
            raise Exception("Cannot attest monitor - not initialized!")
        return _w.attest_monitor()

    # helper functions for attestation microbenchmarks
    def measure_monitor_cold(self):
        if not self.fd:
            raise Exception("Cannot measure monitor - not initialized!")
        return _w.measure_monitor_cold()

    def measure_monitor_hot(self):
        if not self.fd:
            raise Exception("Cannot measure monitor - not initialized!")
        return _w.measure_monitor_hot()

    # end of helper functions for attestation microbenchmarks

    def __enter__(self):
        if not self.fd:
            self.open_device()
        return self

    def __exit__(self, exc_type, exc_value, traceback):
        self.close_device()


class TrustedProcess:
    def __init__(self, process_id):
        self.process_id = process_id


class Trustlet(TrustedProcess):
    RESULT_PENDING = 0xFD

    def __init__(self, process_id):
        TrustedProcess.__init__(self, process_id)

    def invoke_trustlet_bin(self, argument: bytes, output_size: int) -> bytes:
        ret = _w.invoke_trustlet_bin(self.process_id, argument, output_size)
        return ret

    def invoke_trustlet(self, argument: str, output_size: int) -> str:
        ret = _w.invoke_trustlet(self.process_id, argument, output_size)
        return ret

    def load_module(self, wasm_data: bytes) -> int:
        """Load a WASM module into the trustlet. Returns module_id."""
        ret, module_id = _w.trustlet_load_module(self.process_id, wasm_data)
        if ret != 0:
            raise RuntimeError(f"load_module failed: ret={ret}")
        return module_id

    def submit_task(self, module_id: int, func_name: str,
                    argv: List[int] = None) -> int:
        """Submit an async task. Returns request_id."""
        if argv is None:
            argv = []
        ret, request_id = _w.trustlet_submit_task(
            self.process_id, module_id, func_name, argv)
        if ret != 0:
            raise RuntimeError(f"submit_task failed: ret={ret}")
        return request_id

    def get_result(self, request_id: int) -> Tuple[int, int]:
        """
        Non-blocking result query.
        Returns (status, value).
        status == 0: success, value is the return value.
        status == 0xFD: task still pending.
        """
        ret, status, value = _w.trustlet_get_result(
            self.process_id, request_id)
        if ret != 0:
            raise RuntimeError(f"get_result transport error: ret={ret}")
        return (status, value)

    def poll_result(self, request_id: int,
                    timeout: float = 30.0, interval: float = 0.1) -> int:
        """
        Poll until the task completes or timeout.
        Returns the result value on success.
        Raises TimeoutError if the task does not complete in time.
        """
        import time
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            status, value = self.get_result(request_id)
            if status == 0:
                return value
            if status != self.RESULT_PENDING:
                raise RuntimeError(f"Task failed: status={status}")
            time.sleep(interval)
        raise TimeoutError(
            f"Task {request_id} did not complete within {timeout}s")

    def destroy_runtime(self):
        """Destroy the WASM runtime inside the trustlet."""
        ret = _w.trustlet_destroy_runtime(self.process_id)
        if ret != 0:
            raise RuntimeError(f"destroy_runtime failed: ret={ret}")

    def create_channel(self, trustlet):
        ret = _w.create_channel(self.process_id, trustlet.process_id)
        return ret

    def attest_execution(
        self, input: str, input_len: int, output: str, output_len: int
    ) -> str:
        ret = _w.attest_execution(self.process_id, input, input_len, output, output_len)
        return ret

    # helper functions for attestation microbenchmarks
    def prepare_measure_trustlet_cold(self):
        _w.prepare_measure_trustlet_cold(self.process_id)
        return

    def measure_trustlet_cold(self):
        _w.measure_trustlet_cold(self.process_id)
        return

    def measure_trustlet_hot(self):
        _w.measure_trustlet_hot(self.process_id)
        return

    def measure_function(
        self, input: str, input_len: int, output: str, output_len: int
    ):
        _w.measure_function(self.process_id, input, input_len, output, output_len)
        return

    # end of helper functions for attestation microbenchmarks

    def delete(self):
        _w.delete_trustlet(self.process_id)


class Zygote(TrustedProcess):
    def __init__(self, process_id):
        TrustedProcess.__init__(self, process_id)

    def create_trustlet(self, function_code: FileName) -> Trustlet:
        if not Path(function_code).exists():
            raise Exception(f"Function Code {function_code} not found")
        with open(function_code, mode="r") as file:
            function_code = file.read()
        trustlet_id = _w.create_trustlet(self.process_id, function_code)
        if trustlet_id < 0:
            raise Exception(f"Failed to create trustlet")
        return Trustlet(trustlet_id)

    # helper functions for attestation microbenchmarks
    def prepare_measure_zygote_cold(self):
        _w.prepare_measure_zygote_cold(self.process_id)
        return

    def measure_zygote_cold(self):
        _w.measure_zygote_cold(self.process_id)
        return

    def measure_zygote_hot(self):
        _w.measure_zygote_hot(self.process_id)
        return

    # end of helper functions for attestation microbenchmarks

    def delete(self):
        _w.delete_zygote(self.process_id)
