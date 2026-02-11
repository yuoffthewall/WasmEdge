(module
  ;; Recursive fibonacci - tests function calls and recursion
  (func $fib_recursive (export "fib_recursive") (param $n i32) (result i32)
    (if (result i32) (i32.lt_s (local.get $n) (i32.const 2))
      (then (local.get $n))
      (else
        (i32.add
          (call $fib_recursive (i32.sub (local.get $n) (i32.const 1)))
          (call $fib_recursive (i32.sub (local.get $n) (i32.const 2)))
        )
      )
    )
  )

  ;; Iterative fibonacci - tests loops and locals
  (func $fib_iterative (export "fib_iterative") (param $n i32) (result i32)
    (local $a i32)
    (local $b i32)
    (local $temp i32)
    (local $i i32)
    
    (local.set $a (i32.const 0))
    (local.set $b (i32.const 1))
    (local.set $i (i32.const 0))
    
    (block $done
      (loop $loop
        (br_if $done (i32.ge_s (local.get $i) (local.get $n)))
        
        (local.set $temp (local.get $b))
        (local.set $b (i32.add (local.get $a) (local.get $b)))
        (local.set $a (local.get $temp))
        
        (local.set $i (i32.add (local.get $i) (i32.const 1)))
        (br $loop)
      )
    )
    (local.get $a)
  )

  ;; Ackermann function - tests deep recursion
  (func $ackermann (export "ackermann") (param $m i32) (param $n i32) (result i32)
    (if (result i32) (i32.eqz (local.get $m))
      (then (i32.add (local.get $n) (i32.const 1)))
      (else
        (if (result i32) (i32.eqz (local.get $n))
          (then (call $ackermann (i32.sub (local.get $m) (i32.const 1)) (i32.const 1)))
          (else
            (call $ackermann
              (i32.sub (local.get $m) (i32.const 1))
              (call $ackermann (local.get $m) (i32.sub (local.get $n) (i32.const 1)))
            )
          )
        )
      )
    )
  )

  ;; Simple sum - tests basic loop
  (func $sum_to_n (export "sum_to_n") (param $n i32) (result i32)
    (local $sum i32)
    (local $i i32)
    (local.set $sum (i32.const 0))
    (local.set $i (i32.const 1))
    (block $done
      (loop $loop
        (br_if $done (i32.gt_s (local.get $i) (local.get $n)))
        (local.set $sum (i32.add (local.get $sum) (local.get $i)))
        (local.set $i (i32.add (local.get $i) (i32.const 1)))
        (br $loop)
      )
    )
    (local.get $sum)
  )

  ;; GCD using Euclidean algorithm - tests conditional branches
  (func $gcd (export "gcd") (param $a i32) (param $b i32) (result i32)
    (local $temp i32)
    (block $done
      (loop $loop
        (br_if $done (i32.eqz (local.get $b)))
        (local.set $temp (i32.rem_u (local.get $a) (local.get $b)))
        (local.set $a (local.get $b))
        (local.set $b (local.get $temp))
        (br $loop)
      )
    )
    (local.get $a)
  )

  ;; Prime check - tests more complex control flow
  (func $is_prime (export "is_prime") (param $n i32) (result i32)
    (local $i i32)
    
    ;; Handle edge cases
    (if (i32.lt_s (local.get $n) (i32.const 2))
      (then (return (i32.const 0)))
    )
    (if (i32.eq (local.get $n) (i32.const 2))
      (then (return (i32.const 1)))
    )
    (if (i32.eqz (i32.rem_u (local.get $n) (i32.const 2)))
      (then (return (i32.const 0)))
    )
    
    ;; Check odd divisors up to sqrt(n)
    (local.set $i (i32.const 3))
    (block $done
      (loop $loop
        ;; if i*i > n, we're done - n is prime
        (br_if $done (i32.gt_s (i32.mul (local.get $i) (local.get $i)) (local.get $n)))
        ;; if n % i == 0, not prime
        (if (i32.eqz (i32.rem_u (local.get $n) (local.get $i)))
          (then (return (i32.const 0)))
        )
        (local.set $i (i32.add (local.get $i) (i32.const 2)))
        (br $loop)
      )
    )
    (i32.const 1)
  )

  ;; Simple wrapper that just calls is_prime - for debugging call returns
  (func $test_is_prime (export "test_is_prime") (param $n i32) (result i32)
    (call $is_prime (local.get $n))
  )

  ;; Count primes up to n - tests nested control flow
  (func $count_primes (export "count_primes") (param $n i32) (result i32)
    (local $count i32)
    (local $i i32)
    (local.set $count (i32.const 0))
    (local.set $i (i32.const 2))
    (block $done
      (loop $loop
        (br_if $done (i32.gt_s (local.get $i) (local.get $n)))
        (if (call $is_prime (local.get $i))
          (then (local.set $count (i32.add (local.get $count) (i32.const 1))))
        )
        (local.set $i (i32.add (local.get $i) (i32.const 1)))
        (br $loop)
      )
    )
    (local.get $count)
  )

  ;; Memory for array operations (1 page = 64KB)
  (memory (export "memory") 1)

  ;; Simple array operations using memory
  (func $array_sum (export "array_sum") (param $offset i32) (param $len i32) (result i32)
    (local $sum i32)
    (local $i i32)
    (local.set $sum (i32.const 0))
    (local.set $i (i32.const 0))
    (block $done
      (loop $loop
        (br_if $done (i32.ge_s (local.get $i) (local.get $len)))
        (local.set $sum 
          (i32.add 
            (local.get $sum)
            (i32.load (i32.add (local.get $offset) (i32.mul (local.get $i) (i32.const 4))))
          )
        )
        (local.set $i (i32.add (local.get $i) (i32.const 1)))
        (br $loop)
      )
    )
    (local.get $sum)
  )

  ;; Helper: get array element at index (base + index * 4)
  (func $array_get (param $base i32) (param $index i32) (result i32)
    (i32.load (i32.add (local.get $base) (i32.mul (local.get $index) (i32.const 4))))
  )

  ;; Helper: set array element at index
  (func $array_set (param $base i32) (param $index i32) (param $value i32)
    (i32.store 
      (i32.add (local.get $base) (i32.mul (local.get $index) (i32.const 4)))
      (local.get $value)
    )
  )

  ;; Helper: swap two array elements
  (func $swap (param $base i32) (param $i i32) (param $j i32)
    (local $temp i32)
    (local.set $temp (call $array_get (local.get $base) (local.get $i)))
    (call $array_set (local.get $base) (local.get $i) 
      (call $array_get (local.get $base) (local.get $j)))
    (call $array_set (local.get $base) (local.get $j) (local.get $temp))
  )

  ;; Partition function for quicksort (Lomuto partition scheme)
  ;; Returns the final position of the pivot
  (func $partition (export "partition") (param $base i32) (param $low i32) (param $high i32) (result i32)
    (local $pivot i32)
    (local $i i32)
    (local $j i32)
    
    ;; Use last element as pivot
    (local.set $pivot (call $array_get (local.get $base) (local.get $high)))
    (local.set $i (i32.sub (local.get $low) (i32.const 1)))
    (local.set $j (local.get $low))
    
    (block $done
      (loop $loop
        ;; while j < high
        (br_if $done (i32.ge_s (local.get $j) (local.get $high)))
        
        ;; if arr[j] <= pivot
        (if (i32.le_s (call $array_get (local.get $base) (local.get $j)) (local.get $pivot))
          (then
            ;; i++; swap(arr[i], arr[j])
            (local.set $i (i32.add (local.get $i) (i32.const 1)))
            (call $swap (local.get $base) (local.get $i) (local.get $j))
          )
        )
        
        (local.set $j (i32.add (local.get $j) (i32.const 1)))
        (br $loop)
      )
    )
    
    ;; swap(arr[i+1], arr[high]) - put pivot in correct position
    (call $swap (local.get $base) (i32.add (local.get $i) (i32.const 1)) (local.get $high))
    
    ;; return i + 1
    (i32.add (local.get $i) (i32.const 1))
  )

  ;; Quicksort - recursive in-place sort
  ;; Sorts array from index low to high (inclusive)
  (func $quicksort (export "quicksort") (param $base i32) (param $low i32) (param $high i32)
    (local $pivot_idx i32)
    
    ;; Base case: if low >= high, return
    (if (i32.lt_s (local.get $low) (local.get $high))
      (then
        ;; Partition and get pivot index
        (local.set $pivot_idx (call $partition (local.get $base) (local.get $low) (local.get $high)))
        
        ;; Recursively sort left part (low to pivot_idx - 1)
        (call $quicksort (local.get $base) (local.get $low) (i32.sub (local.get $pivot_idx) (i32.const 1)))
        
        ;; Recursively sort right part (pivot_idx + 1 to high)
        (call $quicksort (local.get $base) (i32.add (local.get $pivot_idx) (i32.const 1)) (local.get $high))
      )
    )
  )

  ;; Helper to check if array is sorted
  (func $is_sorted (export "is_sorted") (param $base i32) (param $len i32) (result i32)
    (local $i i32)
    (local.set $i (i32.const 0))
    (block $done
      (loop $loop
        ;; if i >= len - 1, we're done checking
        (br_if $done (i32.ge_s (local.get $i) (i32.sub (local.get $len) (i32.const 1))))
        
        ;; if arr[i] > arr[i+1], not sorted
        (if (i32.gt_s 
              (call $array_get (local.get $base) (local.get $i))
              (call $array_get (local.get $base) (i32.add (local.get $i) (i32.const 1))))
          (then (return (i32.const 0)))
        )
        
        (local.set $i (i32.add (local.get $i) (i32.const 1)))
        (br $loop)
      )
    )
    (i32.const 1)
  )
)
