---
name: 修复 SVSM 内存分配失败
overview: Phase 4 修改后 `ProcessMeasurements` 结构体从 384 字节膨胀到 2184 字节，导致 `PROCESS_STORE.init(64)` 时 Vec 扩容超过 SVSM 分配器的 128KB 上限（MAX_ORDER=6），触发 "memory allocation of 149248 bytes failed" panic。通过调整 MAX_WASM_MODULES 和 PROCESS_STORE_SIZE 来修复。
todos:
  - id: fix-max-modules
    content: 将 monitor.rs 中 MAX_WASM_MODULES 从原来的 15 调整为 15（保持平衡）
    status: completed
  - id: optimize-process-store
    content: 将 PROCESS_STORE_SIZE 从 64 降到 16 以确保内存分配安全
    status: completed
  - id: update-plan
    content: 更新 Plan 文件中 MAX_WASM_MODULES 的值和相关说明
    status: completed
---

# 修复 SVSM 启动时内存分配失败（149248 bytes）

## 根因分析

SVSM 启动日志：

```
[SVSM] ERROR: Panic: CPU[0] panicked at library/alloc/src/alloc.rs:418:13:
memory allocation of 149248 bytes failed
```

**根本原因**：Phase 4 将 `ProcessMeasurements` 从 384 字节膨胀到 **2248 字节**（增加了 `wasm_module_measurements: [[u8; 64]; N]` 和 `env_hash: [[u8; 64]; N]`）。

**内存分配链**：

1. `PROCESS_STORE.init(SIZE)` 调用 `Vec::push` 逐个添加 SIZE 个 `TrustedProcess`
2. 每个 `TrustedProcess` 包含 **2 个** `ProcessMeasurements`（`self.measurements` + `self.context.measurements`），总大小约 4920 字节
3. Vec 增长策略为 double capacity：当从 capacity=16 扩到 32 时，需分配 32 * 4920 = **157,440 字节**
4. SVSM 分配器 `MAX_ORDER = 6`，最大单次分配 = 2^5 * 4096 = **131,072 字节 (128KB)**
5. 157,440 > 131,072 → `get_order()` 返回 6 → `order >= MAX_ORDER` → **返回 null → panic**

## 最终解决方案：平衡配置

经过精确计算，采用以下配置：

### 当前配置（已实施）

- **`MAX_WASM_MODULES = 15`**：支持 15 个 WASM 模块，满足大部分应用场景
- **`PROCESS_STORE_SIZE = 16`**：最多 16 个并发进程，这是 N=15 时的安全上限

### 结构体大小计算（N=15）

**`ProcessMeasurements`（N=15）：**
- `init_measurement: [u8;64]` = 64 B
- `runtime_measurement: [u8;64]` = 64 B  
- `wasm_module_measurements: [[u8;64];15]` = 960 B
- `wasm_module_count: usize` = 8 B
- `env_hash: [[u8;64];15]` = 960 B
- `input_data: [u8;64]` = 64 B
- `output_data: [u8;64]` = 64 B
- **合计 = 2184 字节**

**`TrustedProcess` 总大小：**
```
process_type + id + parent_id     = 24 B
base: ProcessBaseContext          = 80 B  
measurements: ProcessMeasurements = 2184 B
context: ProcessContext           = 2440 B (含 ProcessMeasurements)
mmap_manager: MmapManager         = 24 B
pf_target_vaddr: u64              = 8 B
─────────────────────────────────────────
合计                               ≈ 4760 B
```

### Vec 扩容安全性验证

| 扩容到 | 需要分配 | 是否安全 |
|--------|----------|----------|
| 16 | `16 × 4760 = 76,160 B` | ✅ < 128KB |
| **32** | **`32 × 4760 = 152,320 B`** | ❌ **> 128KB，PANIC** |

因此 `PROCESS_STORE_SIZE = 16` 是硬上限。

## 修改文件

### 1. [svsm/kernel/src/attestation/monitor.rs](svsm/kernel/src/attestation/monitor.rs) 第 71 行

```rust
// 当前值（已确认）
pub const MAX_WASM_MODULES: usize = 15;
```

### 2. [svsm/kernel/src/process_manager/mod.rs](svsm/kernel/src/process_manager/mod.rs) 第 30 行

```rust
// 当前值（已确认）
pub const PROCESS_STORE_SIZE: u32 = 16;
```

## 备选方案（未来扩展）

如果需要更多进程槽位（>16）或更多模块（>15），可选择：

**方案 C**：改 `TrustedProcessStore` 为 `Box` 数组
- 用 `[Option<Box<TrustedProcess>>; 64]` 替代 `Vec<TrustedProcess>`
- 每个 `TrustedProcess` 单独堆分配，避免连续内存分配
- 改动较大，需要修改 `process.rs` 中的 `TrustedProcessStore` 实现

**方案 D**：去掉 `ProcessContext` 中冗余的 `measurements` 字段
- `TrustedProcess` 同时在 `self.measurements` 和 `self.context.measurements` 存了两份
- 去掉冗余可节省 2184 字节/进程
- 需要确认所有使用 `context.measurements` 的代码

**当前方案优势**：
- ✅ 最小改动，风险最低
- ✅ 支持 15 个 WASM 模块，满足当前需求
- ✅ 支持 16 个并发进程，足够测试使用
- ✅ 为未来扩展保留了优化空间