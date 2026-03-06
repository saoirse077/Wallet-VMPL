#!/usr/bin/env python3
"""
test_phase4_attest.py - Phase 4 差分认证完整测试

测试全部 4 种认证类型的调用链：
  Type 0: Monitor 认证
  Type 1: WAMR Runtime 认证
  Type 2: WASM Module 认证
  Type 3: 函数执行认证

前置条件：
  - vmpl.ko 已加载 (insmod vmpl.ko)
  - wallet Python 模块已安装
  - wamr_pal.elf, dummy.manifest, dummy.libos, add.wasm 在当前目录

Debug 模式查看报告文件：
  - 默认编译模式（不定义 NODEBUG）会在当前目录生成报告文件
  - monitor_attestation_report
  - wamr_runtime_attestation_report  
  - wasm_module_attestation_report
  - function_attestation_report0, function_attestation_report1, ...

Usage (inside the guest VM):
  cd /root/module
  insmod vmpl.ko
  make -B -C libwallet/ libwallet.so libwallet.a
  cd python && python3 setup.py install && cd ..
  cd example && python3 test_phase4_attest.py
  ls -la *_report*  # 查看生成的报告文件
"""

import sys
import os
import struct

try:
    import wallet
except ImportError:
    print("ERROR: 'wallet' module not found.")
    print("Please build and install it first:")
    print("  cd /root/module/python && python3 setup.py install")
    sys.exit(1)

# 文件路径（与 test_wamr.py 一致）
WAMR_PAL_ELF = "./wamr_pal.elf"
DUMMY_MANIFEST = "./dummy.manifest"
DUMMY_LIBOS = "./dummy.libos"
WASM_FILE = "./add.wasm"
# [NO-TRUSTLET] DUMMY_FUNCTION 不再需要
# DUMMY_FUNCTION = "./dummy_function.txt"
OUTPUT_SIZE = 4096


def check_files():
    """验证所有必需文件存在"""
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

    # 打印文件大小
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


def pack_shutdown_signal():
    """
    Mode 3: Pack shutdown signal (wasm_size=0, func_name_len=0).
    Signals VMPL1 to unload module, destroy runtime, and exit cleanly.
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


def print_report_hex(report_data, label):
    """打印认证报告的十六进制摘要"""
    if isinstance(report_data, str):
        # Debug 模式返回文件路径
        print(f"  {label}: Report saved to {report_data}")
        if os.path.exists(report_data):
            size = os.path.getsize(report_data)
            print(f"    File size: {size} bytes")
            # 读取前 64 字节并显示十六进制
            with open(report_data, "rb") as f:
                first_bytes = f.read(64)
                print(f"    First 64 bytes: {first_bytes.hex()}")
        else:
            print(f"    WARNING: Report file not found!")
    else:
        # Release 模式返回原始字节
        data = report_data if isinstance(report_data, bytes) else bytes(report_data)
        print(f"  {label}: Report size = {len(data)} bytes")
        # 打印前 64 字节
        print(f"    First 64 bytes: {data[:64].hex()}")


def main():
    print("=" * 70)
    print("Phase 4 差分认证完整测试 (NO-TRUSTLET)")
    print("=" * 70)
    print()

    # ---- Step 1: 检查文件 ----
    print("[1/6] 检查必需文件...")
    check_files()
    print()

    # ---- Step 2: 读取 WASM 模块 ----
    print("[2/6] 读取 WASM 模块...")
    with open(WASM_FILE, "rb") as f:
        wasm_bytes = f.read()
    print(f"  加载 {len(wasm_bytes)} 字节从 {WASM_FILE}")
    print()

    pass_count = 0
    total_count = 0

    with wallet.Wallet() as w:

        # ============================================
        # Test 1: Monitor 认证 (type=0)
        # ============================================
        total_count += 1
        print("[Test 1/4] Monitor 认证 (type=0)")
        print("-" * 50)
        try:
            report = w.attest_monitor()
            print_report_hex(report, "Monitor Report")
            print("  *** PASS: Monitor 认证成功 ***")
            pass_count += 1
        except Exception as e:
            print(f"  *** FAIL: {e} ***")
        print()

        # ============================================
        # Step: Create Zygote
        # ============================================
        print("[3/6] 创建 Zygote（初始化 WAMR runtime）...")
        print(f"  ELF: {WAMR_PAL_ELF}")
        print("  (VMPL1 将: 初始化堆 → 初始化 WAMR → pal_svsm_exit(0) 挂起)")
        try:
            zygote = w.create_zygote(WAMR_PAL_ELF, DUMMY_MANIFEST, DUMMY_LIBOS)
            print(f"  Zygote 创建成功，ID: {zygote.process_id}")
        except Exception as e:
            print(f"  FATAL: {e}")
            sys.exit(1)
        print()

        # ============================================
        # Test 2: WAMR Runtime 认证 (type=1) — 通过 Zygote
        # ============================================
        total_count += 1
        print("[Test 2/4] WAMR Runtime 认证 (type=1) — via Zygote")
        print("-" * 50)
        print("  测试内容: SNP + init_measurement + runtime_measurement")
        try:
            report = zygote.attest_wamr_runtime()
            print_report_hex(report, "WAMR Runtime Report (Zygote)")
            print("  *** PASS: WAMR Runtime 认证成功 ***")
            pass_count += 1
        except Exception as e:
            print(f"  *** FAIL: {e} ***")
        print()

        # [NO-TRUSTLET] Step 4 (Create Trustlet) 和 Test 3 (via Trustlet) 已删除
        # # ============================================
        # # Step: Create Trustlet (CoW)
        # # ============================================
        # print("[4/7] 创建 Trustlet（CoW 复制 Zygote）...")
        # try:
        #     trustlet = zygote.create_trustlet(DUMMY_FUNCTION)
        #     print(f"  Trustlet 创建成功，ID: {trustlet.process_id}")
        # except Exception as e:
        #     print(f"  FATAL: {e}")
        #     sys.exit(1)
        # print()
        #
        # # ============================================
        # # Test 3: WAMR Runtime 认证 (type=1) — 通过 Trustlet
        # # ============================================
        # total_count += 1
        # print("[Test 3/6] WAMR Runtime 认证 (type=1) — via Trustlet")
        # print("-" * 50)
        # print("  测试内容: 验证 CoW 后度量值继承")
        # try:
        #     report = trustlet.attest_wamr_runtime()
        #     print_report_hex(report, "WAMR Runtime Report (Trustlet)")
        #     print("  *** PASS: Trustlet WAMR Runtime 认证成功 ***")
        #     pass_count += 1
        # except Exception as e:
        #     print(f"  *** FAIL: {e} ***")
        # print()

        # ============================================
        # Step: invoke_trustlet_bin (Mode 1: Load + Invoke) — 直接在 Zygote 上
        # 这一步会触发 WASM 模块度量 + env_hash 计算
        # ============================================
        print("[4/6] 调用 add(3, 5) — Mode 1: Load + Invoke (on Zygote)")
        print("  (这会触发 WASM 模块度量 + env_hash 计算)")
        input_data = pack_input_load_and_invoke(wasm_bytes, "add", [3, 5])
        print(f"  输入载荷: {len(input_data)} 字节 (包含 {len(wasm_bytes)} 字节 WASM)")
        try:
            result_bytes = zygote.invoke_trustlet_bin(input_data, OUTPUT_SIZE)
            status, result = parse_output(result_bytes)
            print(f"  输出: status={status}, result={result}")
            if status == 0 and result == 8:
                print("  add(3, 5) = 8 ✓")
            else:
                print(f"  WARNING: 期望 status=0, result=8")
        except Exception as e:
            print(f"  FATAL: {e}")
            sys.exit(1)
        print()

        # ============================================
        # Test 3: WASM Module 认证 (type=2) — 直接在 Zygote 上
        # 必须在 invoke_trustlet_bin (Mode 1) 之后调用
        # ============================================
        total_count += 1
        print("[Test 3/4] WASM Module 认证 (type=2) — module_id=0 (on Zygote)")
        print("-" * 50)
        print("  测试内容: SNP + init + runtime + wasm_module_measurements[0]")
        print("  注意: 必须在 invoke_trustlet_bin (Mode 1) 之后调用")
        try:
            report = zygote.attest_wasm_module(module_id=0)
            print_report_hex(report, "WASM Module Report")
            print("  *** PASS: WASM Module 认证成功 ***")
            pass_count += 1
        except Exception as e:
            print(f"  *** FAIL: {e} ***")
        print()

        # ============================================
        # Test 4: 函数执行认证 (type=3) — 直接在 Zygote 上
        # 必须在 invoke_trustlet_bin 之后调用
        # ============================================
        total_count += 1
        print("[Test 4/4] 函数执行认证 (type=3) (on Zygote)")
        print("-" * 50)
        print("  测试内容: SNP + init + runtime + module + env_hash + input_hash + output_hash + 签名")
        print("  注意: attest_execution 的 input/output 是用户提供的原始数据")
        print("        SVSM 会从 Guest 页表读取这些数据并计算 SHA-512 哈希")
        try:
            # input_data 是我们发送给 invoke_trustlet_bin 的打包数据
            # result_bytes 是 invoke_trustlet_bin 返回的输出
            report = zygote.attest_execution(
                input=input_data,
                input_len=len(input_data),
                output=result_bytes,
                output_len=len(result_bytes),
                module_id=0
            )
            print_report_hex(report, "Function Execution Report")
            print("  *** PASS: 函数执行认证成功 ***")
            pass_count += 1
        except Exception as e:
            print(f"  *** FAIL: {e} ***")
        print()

        # # ============================================
        # # Test 6: 边界测试 — 无效 module_id
        # # ============================================
        # total_count += 1
        # print("[Test 6/6] 边界测试 — WASM Module 认证 with invalid module_id=99")
        # print("-" * 50)
        # print("  测试内容: 验证无效 module_id 的错误处理")
        # try:
        #     report = trustlet.attest_wasm_module(module_id=99)
        #     # 如果 SVSM 返回错误，这里应该会抛异常或返回空报告
        #     print(f"  收到报告 (意外成功或优雅错误处理)")
        #     print("  *** PASS: 优雅处理无效 module_id ***")
        #     pass_count += 1
        # except Exception as e:
        #     print(f"  预期错误: {e}")
        #     print("  *** PASS: 正确抛出错误 ***")
        #     pass_count += 1
        # print()

        # ============================================
        # Cleanup: Send shutdown signal — 直接在 Zygote 上
        # ============================================
        print("[5/6] 发送关闭信号 (on Zygote)...")
        print("  (确保完整资源清理，包括 SVSM 分配)")
        try:
            shutdown_data = pack_shutdown_signal()
            zygote.invoke_trustlet_bin(shutdown_data, OUTPUT_SIZE)
            print("  关闭完成")
        except Exception as e:
            print(f"  关闭信号: {e}")

    print()
    print("=" * 70)
    print(f"Phase 4 认证测试完成: {pass_count}/{total_count} 测试通过")
    if pass_count == total_count:
        print("所有测试通过！")
        print()
        print("关键 Phase 4 功能验证:")
        print("  ✓ Monitor 认证 (type=0) — SNP Report 缓存")
        print("  ✓ WAMR Runtime 认证 (type=1) — init + runtime 度量")
        print("  ✓ WASM Module 认证 (type=2) — 模块哈希度量")
        print("  ✓ 函数执行认证 (type=3) — 完整报告 + 签名")
        # print("  ✓ 边界测试 — 无效参数错误处理")
    else:
        print(f"部分测试失败 ({total_count - pass_count} 个失败)")
    print()
    print("检查 SVSM 串口输出以查看详细日志:")
    print("  - [Phase4] WASM module 0 measured, hash=...")
    print("  - [Phase4] env_hash[0] read from output channel")
    print("  - [Performing WAMR runtime X attestation]")
    print("  - [Performing WASM module attestation, process=X, module=0]")
    print("  - [Performing function execution attestation]")
    print()
    print("Debug 模式报告文件 (当前目录):")
    report_files = [
        "monitor_attestation_report",
        "wamr_runtime_attestation_report",
        "wasm_module_attestation_report",
        "function_attestation_report0"
    ]
    for filename in report_files:
        if os.path.exists(filename):
            size = os.path.getsize(filename)
            print(f"  ✓ {filename} ({size} bytes)")
        else:
            print(f"  ✗ {filename} (not found - may be Release mode)")
    print()
    print("查看报告文件内容:")
    print("  hexdump -C monitor_attestation_report | head -20")
    print("  hexdump -C function_attestation_report0 | head -20")
    print("=" * 70)


if __name__ == "__main__":
    main()