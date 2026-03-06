// Wallet device library for Python
// This only defines a minimal set of functions to interact with the wallet
// device. The main part of the library is implemented in Python (see ./wallet)

#include <fcntl.h>
#include <pybind11/pybind11.h>
#include <sys/ioctl.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>

#define STRINGIFY(x) #x
#define MACRO_STRINGIFY(x) STRINGIFY(x)

namespace py = pybind11;

extern "C" {
#include "../../include/vmpl.h"
#include <attest.h>
#include <attest_microbenchmark.h>
#include <lib.h>
#include <monitor.h>
#include <sys/io.h>
#include <trustlet.h>
#include <zygote.h>
}

bool permission = false;
#define BENCHMARK_PORT 0xF4
void get_permission() {
  if (permission) {
    return;
  }
  if (ioperm(BENCHMARK_PORT, 1, 1)) {
    fprintf(stderr, "Unable to access benchmark port");
  } else {
    permission = true;
  }
}
int count = 0;
void start_measure() { outb(75, BENCHMARK_PORT); }

void end_measure() { outb(76, BENCHMARK_PORT); }

PYBIND11_MODULE(_wallet, m) {
  m.def("monitor_connect", &monitor_connect);
  m.def("monitor_close", &monitor_close);
  m.def("create_zygote", &create_zygote, py::arg("zygote_path"),
        py::arg("manifest_path"), py::arg("libos_path"));
  m.def("delete_zygote", &delete_zygote, py::arg("zygote_id"));
  /* [NO-TRUSTLET] create_trustlet 绑定已禁用 */
  // m.def("create_trustlet", &create_trustlet, py::arg("zygote_id"),
  //       py::arg("function_code"));
  m.def(
      "invoke_trustlet_bin",
      [](const int trustlet_id, std::string args, uint64_t output_size) {
        char *ptr = invoke_trustlet_bin(trustlet_id, args.data(), args.size(),
                                        output_size);
        return py::bytes(std::string(ptr, output_size));
      },
      py::arg("trustlet_id"), py::arg("args"), py::arg("output_size"));
  m.def("invoke_trustlet", &invoke_trustlet, py::arg("trustlet_id"),
        py::arg("args"), py::arg("output_size"));
  /* [NO-TRUSTLET] delete_trustlet 绑定已禁用 */
  // m.def("delete_trustlet", &delete_trustlet, py::arg("trustlet_id"));
  m.def("attest_monitor", &attest_monitor);
  m.def("attest_wamr_runtime", &attest_wamr_runtime,
        py::arg("process_id"));
  m.def("attest_wasm_module", &attest_wasm_module,
        py::arg("process_id"), py::arg("module_id"));
  m.def("attest_execution", &attest_execution, py::arg("trusted_process_id"),
        py::arg("module_id"), py::arg("input"), py::arg("input_len"),
        py::arg("output"), py::arg("output_len"));
  /* helper functions for attestation microbenchmark */
  m.def("measure_monitor_cold", &measure_monitor_cold);
  m.def("measure_monitor_hot", &measure_monitor_hot);
  m.def("prepare_measure_wamr_runtime_cold", &prepare_measure_wamr_runtime_cold,
        py::arg("process_id"));
  m.def("measure_wamr_runtime_cold", &measure_wamr_runtime_cold,
        py::arg("process_id"));
  m.def("measure_wamr_runtime_hot", &measure_wamr_runtime_hot,
        py::arg("process_id"));
  m.def("measure_wasm_module_cold", &measure_wasm_module_cold,
        py::arg("process_id"), py::arg("module_id"));
  m.def("measure_wasm_module_hot", &measure_wasm_module_hot,
        py::arg("process_id"), py::arg("module_id"));
  m.def("measure_function", &measure_function, py::arg("process_id"),
        py::arg("module_id"), py::arg("input"), py::arg("input_len"),
        py::arg("output"), py::arg("output_len"));
  /* end of helper functions for attestation microbenchmark */
  /*Getting status of Wallet #pf, pvalidate, cow*/
  m.def("stat_get", []() {
    get_permission();
    end_measure();
    fprintf(stderr, "End measure %d\n", count);
    stat_get();
  });
  m.def("stat_reset", []() {
    get_permission();
    start_measure();
    fprintf(stderr, "Start measure %d\n", count++);
    stat_reset();
  });
  m.def("create_channel", &create_channel, py::arg("trustlet_id_1"), py::arg("trustlet_id_2"));
#ifdef VERSION_INFO
  m.attr("__version__") = MACRO_STRINGIFY(VERSION_INFO);
#else
  m.attr("__version__") = "dev";
#endif
}
