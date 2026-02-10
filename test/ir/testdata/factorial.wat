(module
  ;; Iterative factorial function
  ;; factorial(n) = n * (n-1) * (n-2) * ... * 1
  (func $factorial (export "factorial") (param $n i32) (result i32)
    (local $result i32)
    ;; Initialize result to 1
    (local.set $result (i32.const 1))
    ;; Loop while n > 1
    (block $exit
      (loop $loop
        ;; if (n <= 1) break
        (br_if $exit (i32.le_s (local.get $n) (i32.const 1)))
        ;; result = result * n
        (local.set $result (i32.mul (local.get $result) (local.get $n)))
        ;; n = n - 1
        (local.set $n (i32.sub (local.get $n) (i32.const 1)))
        ;; continue loop
        (br $loop)
      )
    )
    ;; return result
    (local.get $result)
  )

  ;; Simple add function
  (func $add (export "add") (param $a i32) (param $b i32) (result i32)
    (i32.add (local.get $a) (local.get $b))
  )

  ;; Function that calls other function
  (func $factorial_plus_one (export "factorial_plus_one") (param $n i32) (result i32)
    (i32.add (call $factorial (local.get $n)) (i32.const 1))
  )

  ;; Memory operations test
  (memory (export "memory") 1)
  
  (func $store_and_load (export "store_and_load") (param $offset i32) (param $value i32) (result i32)
    ;; Store value at offset
    (i32.store (local.get $offset) (local.get $value))
    ;; Load and return value at offset
    (i32.load (local.get $offset))
  )

  ;; Global variable test
  (global $counter (mut i32) (i32.const 0))
  
  (func $get_counter (export "get_counter") (result i32)
    (global.get $counter)
  )
  
  (func $increment_counter (export "increment_counter") (result i32)
    (global.set $counter (i32.add (global.get $counter) (i32.const 1)))
    (global.get $counter)
  )
)
