;; add.wat - WebAssembly test module for WAMR-PAL Phase 3
;;
;; Exports three functions for testing the WAMR runtime integration:
;;   - add(a, b)       → a + b
;;   - multiply(a, b)  → a * b
;;   - get_answer()    → 42
;;
;; Compile to binary:
;;   wat2wasm add.wat -o add.wasm
;;
;; If wat2wasm is not installed:
;;   sudo apt-get install wabt
;;   -- or --
;;   pip install wabt

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
