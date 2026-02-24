;; add.wat - Simple WebAssembly test module for WAMR-PAL Phase 2+
;;
;; Exports a single function "add" that takes two i32 arguments and returns
;; their sum. This is the simplest possible serverless function for testing
;; the WAMR runtime integration.
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
    i32.add))
