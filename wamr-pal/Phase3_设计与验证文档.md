# Phase 3 设计文档 — WAMR-PAL 完整 Wallet-VMPL 执行流

## 一、Phase 3 目标

Phase 3 的核心目标是实现一个**完整的、单线程的 Wallet-VMPL 执行流**，使用 WAMR (WebAssembly Micro Runtime) 替代原有的 Gramine-based VMPL1 运行时。

具体来说：
- **VMPL1** 不再运行 Gramine + Python，而是运行一个裸机（bare-metal）WAMR 运行时
- **WASM 字节码**不再嵌入 ELF（Phase 2 做法），而是通过 **input channel** 动态传入
- 执行结果通过 **output channel** 返回给 Guest (VMPL2)
- 结果在 **Guest 终端**显示，不需要查看 SVSM 串口
- **SVSM 侧零修改**，完全复用现有的 `invoke_trustlet` 机制

### Phase 3a vs Phase 3b

| 版本 | 说明 |
|------|------|
| **Phase 3a**（当前版本） | 每次 `invoke_trustlet_bin` 调用都传递完整的 WASM 字节码 + 函数名 + 参数，VMPL1 每次都执行完整的 load module → invoke → unload module 流程 |
| **Phase 3b**（下一版本） | 首次调用传递 WASM 字节码并加载 module，后续调用仅传递函数名 + 参数，VMPL1 复用已加载的 module，只执行 invoke |

## 二、整体架构

```
┌─────────────────────────────────────────────────────────┐
│  VMPL2 (Guest OS)                                       │
│  ┌──────────────────────────────────────────────────┐   │
│  │  python3 test_wamr.py                            │   │
│  │    1. create_zygote(wamr_pal.elf)                │   │
│  │    2. create_trustlet(dummy_function.txt)         │   │
│  │    3. invoke_trustlet_bin(packed_input, 4096)     │   │
│  │    4. 解析返回 bytes → 打印结果                    │   │
│  └──────────────┬───────────────────────┬───────────┘   │
│                 │ ioctl                 ↑ result bytes   │
├─────────────────┼───────────────────────┼───────────────┤
│  VMPL0 (SVSM — 可信 Monitor)                            │
│  ┌──────────────┴───────────────────────┴───────────┐   │
│  │  invoke_trustlet():                              │   │
│  │    ① copy_data_from_guest(invoke_data)           │   │
│  │    ② inflate_input + inflate_output              │   │
│  │    ③ copy_into(wasm_bytes → input channel)       │   │
│  │    ④ ap_create loop (恢复 VMPL1 执行)             │   │
│  │    ⑤ pal_svsm_get_result → copy_out → break     │   │
│  │    ⑥ params.rcx = GETRESULT(1)                   │   │
│  └──────────────┬───────────────────────┬───────────┘   │
│                 │ ap_create             ↑ CPUID trap     │
├─────────────────┼───────────────────────┼───────────────┤
│  VMPL1 (裸机 WAMR Runtime)                               │
│  ┌──────────────┴───────────────────────┴───────────┐   │
│  │  wamr_pal_main():                                │   │
│  │    Phase A (early_invoke):                       │   │
│  │      pal_heap_init() → wasmlet_runtime_init()    │   │
│  │      → pal_svsm_exit(0) [挂起]                    │   │
│  │                                                   │   │
│  │    Phase B (invoke_trustlet 恢复后):               │   │
│  │      for(;;) {                                    │   │
│  │        读 input channel → load module             │   │
│  │        → invoke function → 写 output channel      │   │
│  │        → unload module                            │   │
│  │        → pal_svsm_get_result() [挂起]             │   │
│  │      }                                            │   │
│  └───────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────┘
```

## 三、修改文件清单

| 文件 | 改动类型 | 说明 |
|------|---------|------|
| `wamr-pal/pal_monitor_call.h` | 添加 | `pal_svsm_get_result()` 函数声明 |
| `wamr-pal/pal_monitor_call.c` | 添加 | `pal_svsm_get_result()` 实现 (CPUID `0x4FFFFFF8`) |
| `wamr-pal/wamr_pal_main.c` | **重写** | 两阶段生命周期：init → exit → 被唤醒 → 读 input → load → invoke → unload → 写 output → get_result |
| `wamr-pal/wasmlet_vmpl1.c` | 实现 | 简化版 wasmlet 核心（Runtime/Module/Invocation 三层生命周期） |
| `wamr-pal/mpk_allocator_vmpl1.c` | **修改** | `PKEY_POOL_SIZE` 从 4 改为 1（Phase 3a 单线程单模块优化） |
| `wamr-pal/add.wat` | 更新 | 添加 `multiply` 和 `get_answer` 导出函数 |
| `wamr-pal/Makefile` | 微调 | `deploy` 目标额外复制 `add.wasm` + dummy 文件 |
| `module/example/test_wamr.py` | **重写** | 完整 Wallet 执行流：create_zygote → create_trustlet → invoke_trustlet_bin → 解析返回 bytes → 打印结果 |
| **SVSM 侧** | **无需修改** | 现有 `invoke_trustlet` + `0x4FFFFFF8` handler 完全适用 |

## 四、详细调用逻辑

### 4.1 完整时序图

```
Guest (VMPL2)              SVSM (VMPL0)                    VMPL1 (裸机 WAMR)
     │                          │                                │
     │  ① create_zygote         │                                │
     │  (wamr_pal.elf)          │                                │
     │ ─────────────────────→   │                                │
     │                          │  解析 ELF, 建页表, 创建 VMSA     │
     │                          │  early_invoke():                │
     │                          │    ap_create ──────────────→    │
     │                          │                                │ pal_heap_init()
     │                          │                                │ wasmlet_runtime_init()
     │                          │    ←──── CPUID 0x4FFFFFFE ──── │ pal_svsm_exit(0)
     │                          │  return_value=EXIT              │ [VMSA.RIP 停在 cpuid 后]
     │  ← zygote_id ───────────│                                │
     │                          │                                │
     │  ② create_trustlet       │                                │
     │  (dummy_function.txt)    │                                │
     │ ─────────────────────→   │                                │
     │                          │  CoW 复制 zygote → trustlet     │
     │  ← trustlet_id ─────────│                                │
     │                          │                                │
     │  ③ invoke_trustlet_bin   │                                │
     │  (packed_input, 4096)    │                                │
     │ ─────────────────────→   │                                │
     │                          │  copy_data_from_guest()         │
     │                          │  inflate_input(wasm_size)       │
     │                          │  inflate_output(4096)           │
     │                          │  copy_into(wasm→input channel)  │
     │                          │  ap_create loop:                │
     │                          │    ap_create ──────────────→    │
     │                          │                                │ [从 pal_svsm_exit 返回]
     │                          │                                │ 进入 for(;;) 循环
     │                          │                                │ 读 input channel:
     │                          │                                │   header(12B) + func_name
     │                          │                                │   + argv + wasm_bytes
     │                          │                                │ wasmlet_load_module()
     │                          │                                │ wasmlet_invoke("add",2,[3,5])
     │                          │                                │   → result = 8
     │                          │                                │ write_output_channel(0, 8)
     │                          │                                │ wasmlet_unload_module()
     │                          │    ←──── CPUID 0x4FFFFFF8 ──── │ pal_svsm_get_result()
     │                          │                                │ [VMSA.RIP 停在 cpuid 后]
     │                          │  copy_out(output→guest buffer)  │
     │                          │  return_value = GETRESULT(1)    │
     │                          │  break loop                     │
     │  ← result_bytes ────────│  params.rcx = 1                 │
     │                          │                                │
     │  解析 8 字节:             │                                │
     │  status=0, result=8      │                                │
     │  打印: add(3,5) = 8 ✓    │                                │
     │                          │                                │
     │  ④ (可选) 再次调用        │                                │
     │  invoke_trustlet_bin     │                                │
     │  (新的 packed_input)      │                                │
     │ ─────────────────────→   │                                │
     │                          │  ... 同上流程 ...               │
     │                          │    ap_create ──────────────→    │
     │                          │                                │ [从 pal_svsm_get_result 返回]
     │                          │                                │ 循环回到 for(;;) 顶部
     │                          │                                │ 读新的 input channel ...
```

### 4.2 Phase A — 初始化阶段 (early_invoke)

当 Guest 调用 `create_zygote(wamr_pal.elf, ...)` 时，SVSM 会：
1. 解析 ELF 文件，建立 VMPL1 页表
2. 创建 VMSA（Virtual Machine Save Area），设置入口点为 `pal_start`
3. 调用 `early_invoke()` 通过 `ap_create` 恢复 VMPL1 执行

VMPL1 的 `wamr_pal_main()` 开始执行：

```c
// Phase A: 初始化
pal_heap_init();              // 初始化 dlmalloc mspace (16 MB 堆)
wasmlet_runtime_init();       // 初始化 WAMR runtime + MPK 分配器
pal_svsm_exit(0);             // ← CPUID 0x4FFFFFFE, 挂起 VMPL1
// VMSA.RIP 现在指向 cpuid 指令之后的位置
// 当下一次 ap_create 恢复 VMPL1 时，执行从这里继续
```

**关键点**：`pal_svsm_exit(0)` 通过 CPUID trap 通知 SVSM 初始化完成。SVSM 的 `handle_process_request()` 返回 `false`，`early_invoke` 循环中断，`create_zygote` 返回给 Guest。

### 4.3 Phase B — 调用循环 (invoke_trustlet)

当 Guest 调用 `trustlet.invoke_trustlet_bin(packed_input, output_size)` 时：

**SVSM 侧** (`invoke_trustlet()` in `runtime.rs`):
1. `copy_data_from_guest()` — 从 Guest 读取 `invoke_data` 结构
2. `inflate_input()` — 为 input channel 分配物理页
3. `inflate_output()` — 为 output channel 分配物理页
4. `copy_into()` — 将 `packed_input`（WASM 字节码 + 元数据）复制到 input channel (`0x280_0000_0000`)
5. `ap_create` loop — 恢复 VMPL1 执行

**VMPL1 侧** (`wamr_pal_main()` 的 `for(;;)` 循环):

```c
// 从 pal_svsm_exit() 返回（或从上一次 pal_svsm_get_result() 返回）
for (;;) {
    // 1. 读 input channel (0x280_0000_0000)
    struct input_header hdr;  // wasm_size, func_name_len, argc
    char func_name[128];
    uint32_t argv[16];
    uint8_t *wasm_buf = pal_malloc(hdr.wasm_size);
    // 复制 WASM 字节码到可写堆（WAMR 会原地修改字节码）

    // 2. 加载 WASM 模块
    wasmlet_load_module(wasm_buf, wasm_size);

    // 3. 调用函数
    uint32_t result;
    wasmlet_invoke(func_name, argc, argv, &result);

    // 4. 写结果到 output channel (0x300_0000_0000)
    write_output_channel(status=0, result);

    // 5. 清理本次调用
    wasmlet_unload_module();  // 卸载模块 + 销毁 MPK 域
    pal_free(wasm_buf);       // 释放 WASM 字节码堆缓冲

    // 6. 通知 SVSM 结果已就绪
    pal_svsm_get_result();  // ← CPUID 0x4FFFFFF8
    // SVSM: copy_out → guest buffer, return GETRESULT(1), break loop
    // Guest 收到 result_bytes

    // 7. 当下一次 invoke_trustlet_bin 调用时，ap_create 恢复 VMPL1
    //    pal_svsm_get_result() 返回，循环继续
}
```

### 4.4 SVSM 侧 `pal_svsm_get_result` Handler

```rust
// runtime.rs 第 806-830 行
fn pal_svsm_get_result(&mut self) -> bool {
    // 将 output channel 数据复制到 Guest 提供的 result buffer
    self.process.context.channel.copy_out(
        self.result_addr,
        self.guest_page_table,
        self.result_size as usize);

    // 设置返回值为 GETRESULT (1)
    self.return_value = TrustletReturnType::GETRESULT as u64;

    // 返回 false → invoke_trustlet 的 ap_create loop 中断
    false
}
```

**返回路径**：
```
SVSM: params.rcx = rc.return_value (= 1 = GETRESULT)
  → Guest ioctl 返回 1 (invocationGetValue)
  → Guest C 代码返回 return_buffer 指针
  → Python 获得 bytes
```

## 五、通信协议

### 5.1 Input Channel 协议 (VMPL2 → VMPL1)

地址：`0x280_0000_0000`（由 SVSM 的 `copy_into` 写入）

```
偏移量      类型          字段              说明
───────────────────────────────────────────────────────
[0..3]     uint32_t     wasm_size         WASM 字节码大小（字节）
[4..7]     uint32_t     func_name_len     函数名长度（不含 \0）
[8..9]     uint16_t     argc              i32 参数个数
[10..11]   uint16_t     reserved          保留（填 0）
[12..N]    char[]       func_name         函数名（不含 \0，4 字节对齐）
[N+1..M]   uint32_t[]   argv              函数参数（每个 4 字节）
[M+1..P]   uint8_t[]    wasm_bytes        WASM 字节码
```

**Python 打包示例**：
```python
import struct

func_name = "add"
func_name_bytes = func_name.encode("ascii")
argc = 2
argv = [3, 5]

# Header: wasm_size(4) + func_name_len(4) + argc(2) + reserved(2) = 12 字节
header = struct.pack("<IIHH", len(wasm_bytes), len(func_name_bytes), argc, 0)

# 函数名（4 字节对齐填充）
name_padded_len = (len(func_name_bytes) + 3) & ~3
name_data = func_name_bytes + b"\x00" * (name_padded_len - len(func_name_bytes))

# 参数
argv_data = b""
for arg in argv:
    argv_data += struct.pack("<I", arg)

# 组装
payload = header + name_data + argv_data + wasm_bytes
```

### 5.2 Output Channel 协议 (VMPL1 → VMPL2)

地址：`0x300_0000_0000`（VMPL1 写入，SVSM 的 `copy_out` 复制到 Guest buffer）

```
偏移量      类型          字段        说明
──────────────────────────────────────────
[0..3]     uint32_t     status      0 = 成功, 非零 = 错误码
[4..7]     uint32_t     result      函数返回值 (i32)
```

**错误码定义**：
| status | 含义 |
|--------|------|
| 0 | 成功 |
| 1 | 无效的 func_name_len |
| 2 | 内存分配失败 |
| 3 | WASM 模块加载失败 |
| 4 | 函数调用失败 |

## 六、各文件详细改动

### 6.1 `pal_monitor_call.h` — 添加 `pal_svsm_get_result` 声明

在文件末尾（`pal_svsm_inflate_channel` 声明之前）添加：

```c
/* ========== Trustlet result notification ========== */

/*
 * Notify SVSM that results are ready in the output channel.
 * Call number: 0x4FFFFFF8
 *
 * When VMPL1 calls this during invoke_trustlet, SVSM will:
 *   1. Copy the output channel data to the Guest's return buffer
 *   2. Set return_value = GETRESULT (1)
 *   3. Break the ap_create loop (return false)
 *   4. The Guest's ioctl returns invocationGetValue (1)
 *
 * After this call returns, VMPL1 is suspended until the next
 * invoke_trustlet from the Guest.
 */
void pal_svsm_get_result(void);
```

### 6.2 `pal_monitor_call.c` — 添加 `pal_svsm_get_result` 实现

```c
/* ========== Trustlet result notification ========== */

void pal_svsm_get_result(void) {
    struct monitor_call_data data;
    data.rax = 0x4FFFFFF8;   // CPUID 调用号
    data.rbx = 0;
    data.rcx = 0;
    data.rdx = 0;
    monitor_call(&data);
    /* 返回时意味着下一次 invoke_trustlet 的 ap_create 恢复了 VMPL1 */
}
```

**工作原理**：
1. VMPL1 执行 `cpuid` 指令 → 触发 #VC 异常 → 被 SVSM 拦截
2. SVSM 读取 VMSA.rax = `0x4FFFFFF8`，分发到 `pal_svsm_get_result()` handler
3. Handler 执行 `copy_out`（output channel → Guest buffer），设置 `return_value = GETRESULT`
4. Handler 返回 `false` → `ap_create` loop 中断
5. `invoke_trustlet` 将 `rc.return_value` 写入 `params.rcx`，返回给 Guest
6. Guest 侧 ioctl 返回，Python 获得 result bytes

### 6.3 `wamr_pal_main.c` — 两阶段生命周期

完全重写为两阶段模型：

**Phase A（初始化）**：
- `pal_heap_init()` — 初始化 16 MB dlmalloc mspace
- `wasmlet_runtime_init()` — 初始化 WAMR 解释器 runtime + MPK 分配器
- `pal_svsm_exit(0)` — 挂起，等待 `invoke_trustlet`

**Phase B（调用循环）**：
- 从 `pal_svsm_exit()` 返回后进入 `for(;;)` 主循环
- 每次迭代：
  1. 读 input channel header（12 字节）
  2. 验证 header（wasm_size=0 表示关机信号）
  3. 读函数名（4 字节对齐）
  4. 读参数数组
  5. 复制 WASM 字节码到可写堆（WAMR 会原地修改字节码做 byte-order swap）
  6. `wasmlet_load_module()` — 加载 WASM 模块（创建 MPK 域）
  7. `wasmlet_invoke()` — 实例化 + 执行 + 销毁实例
  8. `write_output_channel()` — 写 status + result 到 output channel
  9. 清理：`wasmlet_unload_module()` + `pal_free(wasm_buf)`
  10. `pal_svsm_get_result()` — 通知 SVSM 结果就绪，挂起
  11. 被下一次 `invoke_trustlet` 唤醒，循环继续

### 6.4 `mpk_allocator_vmpl1.c` — Phase 3a PKEY 池优化

**关键修改**：`PKEY_POOL_SIZE` 从 4 改为 1。

```c
/* PKEY 池最大容量（Phase 3 单线程单模块，只需 1 个 pkey） */
#define PKEY_POOL_SIZE 1
```

**原因**：
- Phase 3a 是单线程单模块，每次只加载一个 WASM 模块，只需要 1 个 pkey 域
- SVSM 的 `MPK_MANAGER` 是全局 static 变量，所有进程共享同一个 pkey 分配器
- 由于不调用 `delete()`，每次 `create_zygote` 都会分配新的 pkey，旧的不释放
- `PKEY_POOL_SIZE=4` 时：每次运行消耗 4 个 pkey → 15/4 ≈ 3 次就耗尽
- `PKEY_POOL_SIZE=1` 时：每次运行消耗 1 个 pkey → 可运行 15 次

### 6.5 `add.wat` — 添加测试函数

从只有 `add` 函数扩展为三个函数：

```wat
(module
  (func (export "add") (param i32 i32) (result i32)
    local.get 0
    local.get 1
    i32.add)
  (func (export "multiply") (param i32 i32) (result i32)
    local.get 0
    local.get 1
    i32.mul)
  (func (export "get_answer") (result i32)
    i32.const 42))
```

### 6.6 `test_wamr.py` — 完整 Wallet 执行流

测试脚本执行完整的 Wallet-VMPL 流程：

1. **检查文件** — 确认 `wamr_pal.elf`、`add.wasm`、`dummy.manifest`、`dummy.libos` 存在
2. **读取 WASM** — 从 `add.wasm` 读取二进制字节码（84 字节）
3. **创建 Zygote** — `w.create_zygote(wamr_pal.elf, dummy.manifest, dummy.libos)`
   - 触发 `early_invoke` → VMPL1 初始化 WAMR → 挂起
4. **创建 Trustlet** — `zygote.create_trustlet(dummy_function.txt)`
   - CoW 复制 zygote（必须这一步，因为 `invoke_trustlet_bin` 是 `Trustlet` 类的方法）
5. **调用 WASM 函数** — 三个测试用例：
   - `add(3, 5)` → 期望结果 8
   - `multiply(4, 7)` → 期望结果 28
   - `get_answer()` → 期望结果 42
6. **解析结果** — 从返回的 8 字节中解析 `status` 和 `result`
7. **打印结果** — 在 Guest 终端显示 PASS/FAIL
8. **不调用 delete** — 与原始 `test.py` 行为一致，避免触发 SVSM 资源回收 bug

**关键设计决策：不调用 delete()**

```python
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
```

### 6.7 `Makefile` — deploy 目标增强

```makefile
deploy: $(TARGET)
    cp $(TARGET) ../module/example/
    # 编译 add.wat → add.wasm（如果 wat2wasm 可用）
    # 复制 add.wasm 到 module/example/
    # 创建 dummy.manifest, dummy.libos, dummy_function.txt
```

## 七、SVSM 侧为什么不需要修改

SVSM 中已有的代码完全支持 Phase 3：

1. **`0x4FFFFFF8` handler** (`pal_svsm_get_result`)：
   - 已存在于 `runtime.rs` 第 458 行
   - 实现在第 806-830 行：执行 `copy_out` + 设置 `GETRESULT` + 返回 `false`

2. **`invoke_trustlet`** 函数：
   - 第 263 行：`inflate_input(function_arg_size)` — 为 input channel 分配页
   - 第 264 行：`inflate_output(result_size)` — 为 output channel 分配页
   - 第 266 行：`copy_into(function_arg, ...)` — 将 Guest 传入的数据复制到 input channel
   - 第 352-360 行：`ap_create` loop — 恢复 VMPL1 执行，处理 CPUID trap
   - 第 361 行：`params.rcx = rc.return_value` — 将结果返回给 Guest

3. **`early_invoke`**：在 `create_zygote` 期间调用，同样使用 `ap_create` loop

## 八、验证步骤

### 前置条件

- AMD SEV-SNP 硬件支持
- 已编译的 SVSM (`svsm/bin/coconut-qemu.igvm`)
- 已编译的 Guest 镜像 (`guest.qcow2`)
- 已安装 `wabt` 工具包（提供 `wat2wasm`）

### 步骤 1：安装 wat2wasm（如果尚未安装）

```bash
# 在宿主机上
sudo apt-get install wabt
# 验证
wat2wasm --version
```

### 步骤 2：编译 add.wat → add.wasm

```bash
cd /home/qyq/Wallet-VMPL/wamr-pal
wat2wasm add.wat -o add.wasm
# 验证
ls -la add.wasm
xxd add.wasm | head    # 应该以 \x00asm 开头（WASM magic number）
```

### 步骤 3：编译 wamr_pal.elf

```bash
cd /home/qyq/Wallet-VMPL/wamr-pal
make clean && make
# 预期输出：
#   Built: wamr_pal.elf
#   Size:  ~220KB bytes
```

验证关键符号：
```bash
readelf -s wamr_pal.elf | grep -E "pal_svsm_get_result|wamr_pal_main|write_output_channel"
# 应该看到：
#   pal_svsm_get_result   — FUNC GLOBAL
#   wamr_pal_main         — FUNC GLOBAL
#   write_output_channel  — FUNC LOCAL
```

### 步骤 4：部署文件到 module/example/

```bash
cd /home/qyq/Wallet-VMPL/wamr-pal
make deploy
# 或手动复制：
cp wamr_pal.elf ../module/example/
cp add.wasm ../module/example/
touch ../module/example/dummy.manifest
touch ../module/example/dummy.libos
echo "dummy" > ../module/example/dummy_function.txt
```

验证部署：
```bash
ls -la ../module/example/wamr_pal.elf ../module/example/add.wasm
ls -la ../module/example/dummy.manifest ../module/example/dummy.libos
ls -la ../module/example/dummy_function.txt
```

### 步骤 5：编译 SVSM（如果有修改）

```bash
cd /home/qyq/Wallet-VMPL
make build_svsm
# 生成 svsm/bin/coconut-qemu.igvm
```

> **注意**：Phase 3 不修改 SVSM 代码，如果你之前已经编译过 SVSM 并且没有修改过 SVSM 代码，可以跳过这一步。

### 步骤 6：启动 Guest VM

```bash
cd /home/qyq/Wallet-VMPL
make run
# 这会启动 QEMU + SVSM + Guest VM
# 等待 Guest 启动完成（出现 login 提示符）
```

**说明**：`make run` 会通过 `-virtfs` 参数将 `module/` 目录以 9p 协议挂载到 Guest 的 `/root/module`。

### 步骤 7：在 Guest 内准备环境

登录 Guest 后执行：

```bash
# 1. 加载 VMPL 内核模块
cd /root/module
insmod vmpl.ko

# 2. 编译 libwallet（如果还没编译）
make -B -C libwallet/ libwallet.so libwallet.a

# 3. 安装 Python wallet 模块（如果还没安装）
cd python && python3 setup.py install && cd ..
```

### 步骤 8：运行 Phase 3a 测试

```bash
cd /root/module/example
python3 test_wamr.py
```

### 步骤 9：验证预期输出

见下方"九、Phase 3a 实际运行记录"。

### 常见问题排查

| 问题 | 可能原因 | 解决方法 |
|------|---------|---------|
| `wallet module not found` | Python wallet 模块未安装 | `cd /root/module/python && python3 setup.py install` |
| `Missing required files: add.wasm` | 未编译 add.wat 或未 deploy | 宿主机执行 `cd wamr-pal && wat2wasm add.wat -o add.wasm && make deploy` |
| `Zygote creation FAILED` | vmpl.ko 未加载 | `insmod /root/module/vmpl.ko` |
| `invoke_trustlet_bin FAILED` | SVSM 未正确处理 CPUID | 检查 SVSM 串口日志，确认 `[WAMR-PAL]` 输出 |
| `status != 0` | WASM 加载或调用失败 | 检查 SVSM 串口中的 `[WAMR-PAL] ERROR:` 信息 |
| Guest 启动后 `/root/module` 为空 | 9p 挂载失败 | 检查 `make run` 的 `-virtfs` 参数 |
| 第 N 次运行 pkey_alloc 失败 | SVSM 全局 pkey 耗尽 | 重启 Guest VM；或减小 `PKEY_POOL_SIZE` |

## 九、Phase 3a 实际运行记录

以下是 Phase 3a 版本成功运行的**一次完整 `python3 test_wamr.py`** 的详细日志分析。

### 9.1 Guest 终端输出（第 1 次运行，Zygote ID=0, Trustlet ID=1）

```
============================================================
WAMR-PAL Phase 3 Test — Complete Wallet-VMPL Execution Flow
============================================================

[1/5] Checking required files...
  WAMR PAL ELF: ./wamr_pal.elf (220792 bytes)
  WASM module: ./add.wasm (84 bytes)

[2/5] Reading WASM module...
  Loaded 84 bytes from ./add.wasm

[3/5] Creating Zygote (initializes WAMR runtime in VMPL1)...
  ELF: ./wamr_pal.elf
  (VMPL1 will: init heap → init WAMR → pal_svsm_exit(0))

Zygote ID: 0
  Zygote created with ID: 0

[4/5] Creating Trustlet (CoW duplicate of Zygote)...
Trying to register Trustlet with Monitor
Trustlet ID: 1
  Trustlet created with ID: 1

[5/5] Invoking WASM functions via invoke_trustlet_bin...

  --- Test 1: add(3, 5) ---
  Input payload: 108 bytes
    Header: wasm_size=84, func='add', argc=2, argv=[3, 5]
  Output: status=0, result=8
  *** PASS: add(3, 5) == 8 ***

  --- Test 2: multiply(4, 7) ---
  Input payload: 112 bytes
  Output: status=0, result=28
  *** PASS: multiply(4, 7) == 28 ***

  --- Test 3: get_answer() ---
  Input payload: 108 bytes
  Output: status=0, result=42
  *** PASS: get_answer() == 42 ***

Skipping cleanup (no delete) — matches original Wallet test.py behavior

============================================================
Phase 3 test completed.

Results are displayed above in the Guest terminal.
Also check SVSM serial console for [WAMR-PAL] debug output.
============================================================
```

### 9.2 SVSM 串口输出详细分析（第 1 次运行的完整日志）

以下逐行分析 SVSM 串口输出，对应一次完整的 `python3 test_wamr.py` 执行。

#### 9.2.1 Phase A — 初始化阶段（create_zygote 触发 early_invoke）

| SVSM 日志 | 动作说明 |
|-----------|---------|
| `allocated memory before creation: 13231407104` | SVSM 记录 create_zygote 前的已分配内存量（约 12.3 GB） |
| `[MPK] VMSA XCR0 PKRU state enabled, XCR0=0x207` | SVSM 创建 VMSA 时启用 XCR0 的 PKRU 状态保存位（bit 9），XCR0=0x207 表示支持 x87+SSE+AVX+PKRU |
| `[MPK] VMSA PKRU initialized to 0x55555554` | VMSA 中 PKRU 初始值设为 0x55555554（pkey 0 可读写，pkey 1-15 禁止访问） |
| `[MPK] CR4.PKE enabled, CR4=0x4506f0` | CR4 寄存器的 PKE 位（bit 22）已启用，允许 PKRU 保护机制生效 |
| `[WAMR-PAL] ================================` | VMPL1 开始执行 `wamr_pal_main()`，打印启动横幅 |
| `[WAMR-PAL] WAMR Runtime Phase 3 Starting (MPK=ON)` | 确认 MPK 隔离模式已开启（`ENABLE_MPK_ISOLATION=1`） |
| `[PKRU] startup: PKRU=0x0000000055555554` | 查询当前 PKRU 值，确认初始状态正确（只有 pkey 0 可访问） |
| `[WAMR-PAL] Initializing heap...` | 开始执行 `pal_heap_init()` |
| `[WAMR-PAL] Initializing heap at 0x0000004000000000 size=16 MB` | 在虚拟地址 0x4000000000 处初始化 16 MB 的 dlmalloc mspace |
| `[WAMR-PAL] Heap initialized successfully (16 MB)` | `pal_svsm_virt_alloc()` 成功，SVSM 为 16 MB 区域分配了物理页并建立了页表映射 |
| `[WAMR-PAL] Initializing WAMR runtime...` | 开始执行 `wasmlet_runtime_init()` |
| `[WASMLET] Initializing runtime...` | 进入 wasmlet 层的运行时初始化 |
| `[MPK] ========== MPK Allocator Init ==========` | MPK 分配器初始化开始 |
| `[MPK] Initializing PKEY pool (max=1)...` | PKEY 池最大容量为 1（Phase 3a 优化后的值） |
| `[MPK] mpk_pkey_alloc_only: allocated pkey=15` | SVSM 侧：从全局 PkeyAllocator bitmap 分配 pkey=15 |
| `[MPK] pkey_alloc: pkey=15` | SVSM 侧：pkey 分配成功确认 |
| `[MPK] Allocated pkey=15` | VMPL1 侧：收到 SVSM 返回的 pkey=15，存入 PKEY 池 |
| `[MPK] PKEY pool initialized: 1 pkeys available` | PKEY 池初始化完成，有 1 个可用 pkey |
| `[MPK] Creating default heap (pkey=0, size=4 MB)...` | 创建 pkey=0 的默认堆（4 MB），用于非隔离的内存分配 |
| `[MPK] Allocator initialized (default heap=4 MB)` | MPK 分配器初始化完成 |
| `[WASMLET] Runtime initialized OK` | WAMR runtime（`wasm_runtime_full_init`）初始化成功 |
| `[WAMR-PAL] WAMR runtime initialized` | 返回 `wamr_pal_main()`，运行时初始化完成 |
| `[PKRU] after runtime_init: PKRU=0x0000000055555554` | 确认运行时初始化后 PKRU 状态未被破坏 |
| `[WAMR-PAL] Initialization complete, suspending...` | 准备调用 `pal_svsm_exit(0)` 挂起 VMPL1 |
| `Exit with Status Code: 0` | SVSM 收到 CPUID 0x4FFFFFFE（exit），状态码 0 表示成功 |
| `allocated memory after zygote creation: 13252800512` | create_zygote 完成，已分配内存增加约 20 MB（ELF 加载 + 16 MB 堆 + 页表等） |

#### 9.2.2 create_trustlet（CoW 复制）

| SVSM 日志 | 动作说明 |
|-----------|---------|
| `allocated memory before creation: 13252800512` | create_trustlet 前内存量 |
| `WARN: [handle_cow] the page not marked as CoW, skip` | CoW 处理时发现某页未标记为 CoW，跳过（正常，因为我们的 ELF 不依赖 CoW 机制） |
| `allocated memory after trustlet creation: 13252837376` | create_trustlet 完成，仅增加约 36 KB（新 VMSA + 少量页表页） |

#### 9.2.3 Phase B — 第 1 次函数调用：add(3, 5)

**invoke_trustlet_bin 恢复 VMPL1**：

| SVSM 日志 | 动作说明 |
|-----------|---------|
| `[WAMR-PAL] ================================` | VMPL1 从 `pal_svsm_exit(0)` 返回，进入 Phase B |
| `[WAMR-PAL] Entered invocation loop` | 进入 `for(;;)` 主循环 |

**读取 input channel**：

| SVSM 日志 | 动作说明 |
|-----------|---------|
| `[WAMR-PAL] Input: wasm_size=84, func_name_len=3, argc=2` | 从 input channel (0x280_0000_0000) 读取 header：WASM 84 字节，函数名 3 字节（"add"），2 个参数 |
| `[WAMR-PAL] Function: add` | 读取函数名 "add" |
| `[WAMR-PAL] WASM bytecode at input offset 24, size=84` | WASM 字节码在 input 偏移 24 处（header 12B + "add" 4B 对齐 + 2×4B 参数 = 24B） |

**加载 WASM 模块（创建 MPK 域）**：

| SVSM 日志 | 动作说明 |
|-----------|---------|
| `[WAMR-PAL] Loading WASM module...` | 开始加载 WASM 模块 |
| `[PKRU] before module_load: PKRU=0x0000000055555554` | 加载前确认 PKRU 状态（pkey 0 可访问，其他禁止） |
| `[WASMLET] Loading module (84 bytes)...` | 进入 `wasmlet_load_module()`，84 字节 WASM |
| `[MPK] Creating domain (requested size=4096 KB)...` | 创建 4 MB 的 MPK 隔离域 |
| `[MPK] Allocating region: pkey=15, size=4096 KB` | 从 PKEY 池取出 pkey=15，请求 SVSM 分配 4 MB 内存 |
| `[MPK] mpk_alloc_memory: addr=0x6000400000, size=4194304, pkey=15, pages=1024` | SVSM 侧：在 0x6000400000 分配 1024 页（4 MB），标记 pkey=15 |
| `[MPK] alloc success: addr=0x6000400000, size=4194304, pkey=15` | SVSM 侧：分配成功 |
| `[MPK] Domain created: pkey=15, base=0x6000400000, size=4096 KB` | VMPL1 侧：域创建完成 |
| `[MPK] Entering domain: pkey=15` | 进入 pkey=15 域（启用该 pkey 的读写权限） |
| `[MPK] enter_domain: pkey=15, PKRU 0x55555554 -> 0x15555554` | SVSM 修改 VMSA.PKRU：将 pkey 15 的 2 bit 从 01（禁止写）改为 00（允许读写） |
| `[MPK] Creating module mspace (first enter)...` | 首次进入域时惰性创建 mspace（dlmalloc 独立堆） |
| `[MPK] Module mspace created OK` | 在 pkey=15 保护的 4 MB 区域内创建 mspace 成功 |
| `[MPK] Domain entered OK: pkey=15` | 域进入完成 |
| `[MPK] Exiting domain: pkey=15` | 退出域（恢复 PKRU，禁止 pkey=15 访问） |
| `[MPK] exit_domain: pkey=15, PKRU 0x15555554 -> 0x55555554` | SVSM 恢复 VMSA.PKRU 为默认值 |
| `[MPK] Domain exited OK` | 域退出完成 |
| `[WASMLET] Module loaded OK` | `wasm_runtime_load()` 成功，WASM 模块已解析到 pkey=15 域内 |
| `[WAMR-PAL] Module loaded OK` | 返回 `wamr_pal_main()` |
| `[PKRU] after module_load: PKRU=0x0000000055555554` | 确认模块加载后 PKRU 已恢复为默认值 |

**调用函数 add(3, 5)**：

| SVSM 日志 | 动作说明 |
|-----------|---------|
| `[WAMR-PAL] Invoking add(3, 5)...` | 准备调用 add 函数，参数 3 和 5 |
| `[WASMLET] Invoking 'add'...` | 进入 `wasmlet_invoke()` |
| `[MPK] Entering domain: pkey=15` | 进入 pkey=15 域（实例化和执行都在域内） |
| `[MPK] enter_domain: pkey=15, PKRU 0x55555554 -> 0x15555554` | PKRU 启用 pkey=15 读写 |
| `[MPK] Domain entered OK: pkey=15` | 域进入成功 |
| `[WASMLET] Instantiating module...` | `wasm_runtime_instantiate()` 创建模块实例（分配线性内存等） |
| `[WASMLET] Call succeeded, result=8` | `wasm_runtime_call_wasm()` 执行成功，add(3,5)=8 |
| `[WASMLET] Instance destroyed` | `wasm_runtime_deinstantiate()` 销毁实例 + `wasm_runtime_destroy_exec_env()` 销毁执行环境 |
| `[MPK] Exiting domain: pkey=15` | 退出域 |
| `[MPK] exit_domain: pkey=15, PKRU 0x15555554 -> 0x55555554` | PKRU 恢复 |
| `[MPK] Domain exited OK` | 域退出完成 |

**写结果到 output channel**：

| SVSM 日志 | 动作说明 |
|-----------|---------|
| `[WAMR-PAL] Result: 8` | 打印函数返回值 |
| `[PKRU] after invoke: PKRU=0x0000000055555554` | 确认调用后 PKRU 状态正确 |
| `[WAMR-PAL] Output written: status=0, result=8` | 将 status=0（成功）和 result=8 写入 output channel (0x300_0000_0000) |

**卸载模块（销毁 MPK 域）**：

| SVSM 日志 | 动作说明 |
|-----------|---------|
| `[WASMLET] Unloading module...` | 开始 `wasmlet_unload_module()` |
| `[MPK] Entering domain: pkey=15` | **第 1 次进入域**：进入 pkey=15 域以执行 `wasm_runtime_unload()`（需要访问域内的模块元数据内存） |
| `[MPK] enter_domain: pkey=15, PKRU 0x55555554 -> 0x15555554` | 启用 pkey=15 读写 |
| `[MPK] Domain entered OK: pkey=15` | 进入成功 |
| （`wasm_runtime_unload(g_module)` 在此执行，释放域内的 WASM 解析结构） | |
| `[MPK] Exiting domain: pkey=15` | **第 1 次退出域**：`wasm_runtime_unload` 完成后退出域（恢复 PKRU） |
| `[MPK] exit_domain: pkey=15, PKRU 0x15555554 -> 0x55555554` | PKRU 恢复 |
| `[MPK] Domain exited OK` | 退出成功 |
| `[MPK] Destroying domain: pkey=15` | 开始 `mpk_domain_destroy()`，销毁整个 MPK 域 |
| `[MPK] Temporarily entering domain to destroy mspace...` | **第 2 次进入域**：需要临时进入域以销毁 mspace（`destroy_mspace()` 需要访问域内内存来释放 mspace 的内部数据结构） |
| `[MPK] Entering domain: pkey=15` | 进入域 |
| `[MPK] enter_domain: pkey=15, PKRU 0x55555554 -> 0x15555554` | PKRU 启用 |
| `[MPK] Domain entered OK: pkey=15` | 进入成功 |
| `[MPK] mspace destroyed. Exiting domain...` | `destroy_mspace()` 完成 |
| `[MPK] Exiting domain: pkey=15` | **第 2 次退出域** |
| `[MPK] exit_domain: pkey=15, PKRU 0x15555554 -> 0x55555554` | PKRU 恢复 |
| `[MPK] Domain exited OK` | 退出成功 |
| `[MPK] Unmapping region: base=0x6000400000, size=4194304, pkey=15` | 调用 `mpk_region_unmap_pkey()`：请求 SVSM 释放 4 MB 物理内存 |
| `[MPK] mpk_free_memory: addr=0x6000400000, size=4194304, pkey=15 (pkey retained)` | SVSM 侧：释放物理页，但**保留 pkey**（不释放 pkey bitmap 中的位） |
| `[MPK] free_memory success: addr=0x6000400000, size=4194304` | 释放成功 |
| `[MPK] Domain destroyed: pkey=15` | 域销毁完成，pkey=15 归还到 VMPL1 本地 PKEY 池 |
| `[WASMLET] Module unloaded` | 模块卸载完成 |

**通知 SVSM 结果就绪并挂起**：

| SVSM 日志 | 动作说明 |
|-----------|---------|
| `[WAMR-PAL] Calling pal_svsm_get_result()...` | 调用 CPUID 0x4FFFFFF8，通知 SVSM 结果已写入 output channel |
| （SVSM 执行 `copy_out`：将 output channel 的 8 字节复制到 Guest 的 return_buffer） | |
| （SVSM 设置 `return_value = GETRESULT(1)`，`ap_create` loop 中断） | |
| （Guest 的 ioctl 返回，Python 收到 result_bytes = b'\x00\x00\x00\x00\x08\x00\x00\x00'） | |

#### 9.2.4 Phase B — 第 2 次函数调用：multiply(4, 7)

VMPL1 从 `pal_svsm_get_result()` 返回，循环继续。

| SVSM 日志 | 动作说明 |
|-----------|---------|
| `[WAMR-PAL] Resumed for next invocation` | 从 `pal_svsm_get_result()` 返回，准备处理下一个调用 |
| `[WAMR-PAL] Input: wasm_size=84, func_name_len=8, argc=2` | 读取新的 input：函数名 8 字节（"multiply"），2 个参数 |
| `[WAMR-PAL] Function: multiply` | 函数名 "multiply" |
| `[WAMR-PAL] WASM bytecode at input offset 28, size=84` | WASM 在偏移 28 处（header 12B + "multiply" 8B 对齐 + 2×4B = 28B） |
| `[WAMR-PAL] Loading WASM module...` | 加载模块 |
| `[MPK] Creating domain (requested size=4096 KB)...` | 创建新的 4 MB MPK 域 |
| `[MPK] Allocating region: pkey=15, size=4096 KB` | 复用 pkey=15（已归还到池中） |
| `[MPK] mpk_alloc_memory: addr=0x6000800000, size=4194304, pkey=15` | 新域分配在 0x6000800000（上一个域释放后的下一个地址） |
| `[MPK] alloc success: addr=0x6000800000, size=4194304, pkey=15` | 分配成功 |
| `[MPK] Domain created: pkey=15, base=0x6000800000, size=4096 KB` | 域创建完成 |
| （后续进入域、创建 mspace、退出域、加载模块的日志与 add 完全一致，此处省略） | |
| `[WASMLET] Module loaded OK` | 模块加载成功 |
| `[WAMR-PAL] Invoking multiply(4, 7)...` | 调用 multiply |
| `[WASMLET] Call succeeded, result=28` | multiply(4,7)=28 |
| `[WAMR-PAL] Output written: status=0, result=28` | 写入 output channel |
| （卸载模块、销毁域的日志与 add 完全一致） | |
| `[MPK] mpk_free_memory: addr=0x6000800000, size=4194304, pkey=15 (pkey retained)` | 释放 0x6000800000 的 4 MB |
| `[WAMR-PAL] Calling pal_svsm_get_result()...` | 通知 SVSM 并挂起 |

#### 9.2.5 Phase B — 第 3 次函数调用：get_answer()

| SVSM 日志 | 动作说明 |
|-----------|---------|
| `[WAMR-PAL] Resumed for next invocation` | 从上一次 `pal_svsm_get_result()` 返回 |
| `[WAMR-PAL] Input: wasm_size=84, func_name_len=10, argc=0` | 函数名 10 字节（"get_answer"），0 个参数 |
| `[WAMR-PAL] Function: get_answer` | 函数名 "get_answer" |
| `[WAMR-PAL] WASM bytecode at input offset 24, size=84` | WASM 在偏移 24 处（header 12B + "get_answer" 12B 对齐 + 0 参数 = 24B） |
| `[MPK] mpk_alloc_memory: addr=0x6000c00000, size=4194304, pkey=15` | 第 3 个域分配在 0x6000c00000 |
| `[WASMLET] Call succeeded, result=42` | get_answer()=42 |
| `[WAMR-PAL] Output written: status=0, result=42` | 写入 output channel |
| `[MPK] mpk_free_memory: addr=0x6000c00000, size=4194304, pkey=15 (pkey retained)` | 释放 4 MB |
| `[WAMR-PAL] Calling pal_svsm_get_result()...` | 通知 SVSM 并挂起 |

#### 9.2.6 下一次运行的 create_zygote

当 Guest 再次执行 `python3 test_wamr.py` 时，会创建新的 Zygote：

| SVSM 日志 | 动作说明 |
|-----------|---------|
| `allocated memory before creation: 13252861952` | 第 2 次运行前的内存量（比第 1 次运行后增加了约 60 KB，因为第 1 次运行的 Zygote/Trustlet 未删除） |
| `[MPK] VMSA XCR0 PKRU state enabled, XCR0=0x207` | 新 Zygote 的 VMSA 创建 |
| `[MPK] VMSA PKRU initialized to 0x55555554` | PKRU 初始化 |
| `[MPK] CR4.PKE enabled, CR4=0x4506f0` | CR4.PKE 启用 |
| `[MPK] Initializing PKEY pool (max=1)...` | 新 VMPL1 实例初始化 PKEY 池 |
| `[MPK] mpk_pkey_alloc_only: allocated pkey=14` | SVSM 分配 pkey=14（pkey=15 已被第 1 次运行占用且未释放） |
| `[MPK] Allocated pkey=14` | 新实例获得 pkey=14 |
| `[WASMLET] Runtime initialized OK` | 运行时初始化成功 |

### 9.3 连续运行记录

Phase 3a 版本成功连续运行了 **15 次** `python3 test_wamr.py`，每次都正确返回：
- `add(3, 5) = 8` ✓
- `multiply(4, 7) = 28` ✓
- `get_answer() = 42` ✓

**PKEY 分配记录**：

| 运行次数 | Zygote ID | Trustlet ID | 分配的 pkey | 状态 |
|---------|-----------|-------------|------------|------|
| 第 1 次 | 0 | 1 | pkey=15 | ✓ PASS |
| 第 2 次 | 2 | 3 | pkey=14 | ✓ PASS |
| 第 3 次 | 4 | 5 | pkey=13 | ✓ PASS |
| 第 4 次 | 6 | 7 | pkey=12 | ✓ PASS |
| 第 5 次 | 8 | 9 | pkey=11 | ✓ PASS |
| 第 6 次 | 10 | 11 | pkey=10 | ✓ PASS |
| 第 7 次 | 12 | 13 | pkey=9 | ✓ PASS |
| 第 8 次 | 14 | 15 | pkey=8 | ✓ PASS |
| 第 9 次 | 16 | 17 | pkey=7 | ✓ PASS |
| 第 10 次 | 18 | 19 | pkey=6 | ✓ PASS |
| 第 11 次 | 20 | 21 | pkey=5 | ✓ PASS |
| 第 12 次 | 22 | 23 | pkey=4 | ✓ PASS |
| 第 13 次 | 24 | 25 | pkey=3 | ✓ PASS |
| 第 14 次 | 26 | 27 | pkey=2 | ✓ PASS |
| 第 15 次 | 28 | 29 | pkey=1 | ✓ PASS |
| 第 16 次 | 30 | 31 | **失败** | pkey_alloc error=6（所有 pkey 已耗尽） |

**第 16 次运行失败日志**：
```
[SVSM] ERROR: [MPK] pkey_alloc failed: error=6
[SVSM]  [Trustlet] [MPK] pkey_alloc returned -6, stopping
[SVSM]  [Trustlet] [MPK] Failed to allocate any PKEY from SVSM
[SVSM]  [Trustlet] [WASMLET] MPK allocator init failed
[SVSM]  [Trustlet] [WAMR-PAL] FATAL: WAMR runtime init failed
[SVSM]  [Trustlet] Exit with Status Code: 1
```

**失败原因**：SVSM 的 `MPK_MANAGER` 是全局 static 变量，pkey bitmap 共 15 个可用位（pkey 1-15）。由于不调用 `delete()`，旧进程的 pkey 不会释放。15 次运行后所有 pkey 耗尽。

### 9.4 MPK 域生命周期总结（单次函数调用）

每次函数调用（如 `add(3,5)`）的 MPK 域操作序列：

```
wasmlet_load_module():
  ① mpk_domain_create()        → 分配 pkey + 分配 4MB 内存区域
  ② mpk_domain_enter()         → PKRU 启用 pkey（惰性创建 mspace）
  ③ wasm_runtime_load()        → 在域内解析 WASM（内存分配走域堆）
  ④ mpk_domain_exit()          → PKRU 恢复

wasmlet_invoke():
  ⑤ mpk_domain_enter()         → PKRU 启用 pkey
  ⑥ wasm_runtime_instantiate() → 在域内创建实例
  ⑦ wasm_runtime_call_wasm()   → 执行 WASM 函数
  ⑧ wasm_runtime_deinstantiate() → 销毁实例
  ⑨ mpk_domain_exit()          → PKRU 恢复

wasmlet_unload_module():
  ⑩ mpk_domain_enter()         → 进入域以释放模块内存
  ⑪ wasm_runtime_unload()      → 释放域内的 WASM 解析结构
  ⑫ mpk_domain_exit()          → 退出域

  mpk_domain_destroy():
  ⑬ mpk_domain_enter()         → 临时进入域以销毁 mspace
  ⑭ destroy_mspace()           → 释放 mspace 内部数据结构
  ⑮ mpk_domain_exit()          → 退出域
  ⑯ mpk_region_unmap_pkey()    → 请求 SVSM 释放 4MB 物理内存
  ⑰ pkey_pool_free()           → 归还 pkey 到 VMPL1 本地池
```

## 十、Phase 3 vs Phase 2 对比

| 特性 | Phase 2 | Phase 3a |
|------|---------|---------|
| WASM 字节码来源 | 嵌入 ELF `.rodata` | 通过 input channel 动态传入 |
| 函数名 | 硬编码 `"add"` | 由 input channel 指定 |
| 参数 | 硬编码 `[3, 5]` | 由 input channel 指定 |
| 结果输出 | SVSM 串口 `debug_print` | Guest 终端 (Python 打印) |
| 生命周期 | 一次性执行 | 初始化 + 循环调用 |
| Guest 交互 | 仅 `create_zygote` | `create_zygote` + `create_trustlet` + `invoke_trustlet_bin` |
| 多次调用 | 不支持 | 支持（循环） |
| MPK PKEY 池大小 | 4 | 1（优化后） |
| 模块复用 | N/A | 不复用（每次 load+unload） |
| 最大连续运行次数 | 1 | 15（受 pkey 总数限制） |

## 十一、已知限制与待解决问题

### 11.1 SVSM 资源回收不完整

调用 `delete()` 会触发 SVSM 的 `TrustedProcess::drop()`，但该实现存在以下问题：
- **Zygote Drop 未释放 VMSA 页**
- **VMSA RMP 标记未清除**
- **MPK_MANAGER 状态未随进程删除清理**
- **VMPL1 bump allocator 地址空间不回收**
- **未执行 VMSA disable + TLB flush**

**短期方案**（当前采用）：不调用 `delete()`，每次运行消耗 2 个 PROCESS_STORE slot + 1 个 pkey。

**长期方案**（Phase 4 或后续）：修复 SVSM 侧的 `Drop` 实现，正确回收所有资源。

### 11.2 pkey 耗尽限制

当前最多连续运行 15 次（pkey 1-15 各消耗 1 个）。重启 Guest VM 可重置。

## 十二、Phase 3b 展望

Phase 3b 将优化函数调用模式，**首次调用加载 module，后续调用复用已加载的 module**：

### 12.1 Phase 3a vs Phase 3b 对比

| 特性 | Phase 3a（当前） | Phase 3b（下一版） |
|------|-----------------|-----------------|
| 每次调用传递 WASM 字节码 | ✓ 每次都传 | 仅首次传递 |
| 每次调用 load/unload module | ✓ 每次都做 | 仅首次 load，最后 unload |
| MPK 域创建/销毁 | 每次函数调用 | 仅首次创建，最后销毁 |
| 适用场景 | 不同函数模块的独立调用 | 同一模块的多次函数调用 |

### 12.2 Phase 3b 执行流设想

```
Guest:
  invoke_trustlet_bin(wasm + "add" + [3,5])   → 首次：load module + invoke add
  invoke_trustlet_bin("add" + [10, 20])        → 后续：仅 invoke add（不传 wasm）
  invoke_trustlet_bin("add" + [100, 200])      → 后续：仅 invoke add
  invoke_trustlet_bin(shutdown)                 → 卸载 module
```

### 12.3 Input Channel 协议变更（Phase 3b）

```
wasm_size = 0 且 func_name_len > 0  → 仅调用函数（module 已加载）
wasm_size > 0                        → 加载新 module + 调用函数
wasm_size = 0 且 func_name_len = 0   → 关机信号
```

## 十三、Phase 4 展望

Phase 4 将在 Phase 3 的基础上添加：
- **多线程支持**：多个 vCPU 并发执行不同的 serverless 函数
- **MPK 完整隔离**：每个函数模块使用独立的 pkey 域
- **临时堆管理**：每次函数调用使用临时 mspace，调用结束后销毁
- **TLS 管理**：`tls_current_msp` 用于线程级别的堆选择
- **SVSM 资源回收修复**：正确实现 `TrustedProcess::drop()`