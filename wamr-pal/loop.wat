(module
  ;; 斐波那契数列（迭代版本）
  (func $fib (export "fib") (param $n i32) (result i32)
    (local $a i32)
    (local $b i32)
    (local $tmp i32)
    (local $i i32)
    
    ;; fib(0) = 0, fib(1) = 1
    (if (i32.le_s (local.get $n) (i32.const 1))
      (then (return (local.get $n)))
    )
    
    (local.set $a (i32.const 0))
    (local.set $b (i32.const 1))
    (local.set $i (i32.const 2))
    
    (block $break
      (loop $continue
        (local.set $tmp (i32.add (local.get $a) (local.get $b)))
        (local.set $a (local.get $b))
        (local.set $b (local.get $tmp))
        (local.set $i (i32.add (local.get $i) (i32.const 1)))
        (br_if $continue (i32.le_s (local.get $i) (local.get $n)))
      )
    )
    
    (local.get $b)
  )
  
  ;; 阶乘（迭代版本）
  (func $factorial (export "factorial") (param $n i32) (result i32)
    (local $result i32)
    (local $i i32)
    
    (if (i32.le_s (local.get $n) (i32.const 1))
      (then (return (i32.const 1)))
    )
    
    (local.set $result (i32.const 1))
    (local.set $i (i32.const 2))
    
    (block $break
      (loop $continue
        (local.set $result (i32.mul (local.get $result) (local.get $i)))
        (local.set $i (i32.add (local.get $i) (i32.const 1)))
        (br_if $continue (i32.le_s (local.get $i) (local.get $n)))
      )
    )
    
    (local.get $result)
  )
  
  ;; 求和：1 + 2 + ... + n
  (func $sum_to_n (export "sum_to_n") (param $n i32) (result i32)
    (local $sum i32)
    (local $i i32)
    
    (local.set $sum (i32.const 0))
    (local.set $i (i32.const 1))
    
    (block $break
      (loop $continue
        (local.set $sum (i32.add (local.get $sum) (local.get $i)))
        (local.set $i (i32.add (local.get $i) (i32.const 1)))
        (br_if $continue (i32.le_s (local.get $i) (local.get $n)))
      )
    )
    
    (local.get $sum)
  )
  
  ;; 判断素数
  (func $is_prime (export "is_prime") (param $n i32) (result i32)
    (local $i i32)
    
    (if (i32.le_s (local.get $n) (i32.const 1))
      (then (return (i32.const 0)))
    )
    (if (i32.le_s (local.get $n) (i32.const 3))
      (then (return (i32.const 1)))
    )
    (if (i32.eqz (i32.rem_s (local.get $n) (i32.const 2)))
      (then (return (i32.const 0)))
    )
    
    (local.set $i (i32.const 3))
    (block $break
      (loop $continue
        (if (i32.eqz (i32.rem_s (local.get $n) (local.get $i)))
          (then (return (i32.const 0)))
        )
        (local.set $i (i32.add (local.get $i) (i32.const 2)))
        (br_if $continue (i32.le_s (i32.mul (local.get $i) (local.get $i)) (local.get $n)))
      )
    )
    
    (i32.const 1)
  )
)
