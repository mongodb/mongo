## 1. 3-Node graph, base node fully connected
### No join opt
### Random reordering with seed 0
`"HJ( _ = ( HJ( _ = ( COLLSCAN [test.base_coll_reorder_md_base] ), y = ( COLLSCAN [test.base_coll_reorder_md_b] ) ) ), x = ( COLLSCAN [test.base_coll_reorder_md_a] ) )"`
```
HASH_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.base_coll_reorder_md_a]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [b = b]
  leftEmbeddingField: "none"
  rightEmbeddingField: "y"
  |  |
  |  COLLSCAN [test.base_coll_reorder_md_b]
  |  direction: "forward"
  |
  COLLSCAN [test.base_coll_reorder_md_base]
  direction: "forward"
```
### Random reordering with seed 1
`"HJ( _ = ( HJ( _ = ( COLLSCAN [test.base_coll_reorder_md_base] ), x = ( COLLSCAN [test.base_coll_reorder_md_a] ) ) ), y = ( COLLSCAN [test.base_coll_reorder_md_b] ) )"`
```
HASH_JOIN_EMBEDDING [b = b]
leftEmbeddingField: "none"
rightEmbeddingField: "y"
  |  |
  |  COLLSCAN [test.base_coll_reorder_md_b]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [a = a]
  leftEmbeddingField: "none"
  rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.base_coll_reorder_md_a]
  |  direction: "forward"
  |
  COLLSCAN [test.base_coll_reorder_md_base]
  direction: "forward"
```
### Random reordering with seed 2
`"HJ( y = ( COLLSCAN [test.base_coll_reorder_md_b] ), _ = ( HJ( x = ( COLLSCAN [test.base_coll_reorder_md_a] ), _ = ( COLLSCAN [test.base_coll_reorder_md_base] ) ) ) )"`
```
HASH_JOIN_EMBEDDING [b = b]
leftEmbeddingField: "y"
rightEmbeddingField: "none"
  |  |
  |  HASH_JOIN_EMBEDDING [a = a]
  |  leftEmbeddingField: "x"
  |  rightEmbeddingField: "none"
  |  |  |
  |  |  COLLSCAN [test.base_coll_reorder_md_base]
  |  |  direction: "forward"
  |  |
  |  COLLSCAN [test.base_coll_reorder_md_a]
  |  direction: "forward"
  |
  COLLSCAN [test.base_coll_reorder_md_b]
  direction: "forward"
```
### Random reordering with seed 3
`"HJ( y = ( COLLSCAN [test.base_coll_reorder_md_b] ), _ = ( NLJ( x = ( COLLSCAN [test.base_coll_reorder_md_a] ), _ = ( COLLSCAN [test.base_coll_reorder_md_base] ) ) ) )"`
```
HASH_JOIN_EMBEDDING [b = b]
leftEmbeddingField: "y"
rightEmbeddingField: "none"
  |  |
  |  NESTED_LOOP_JOIN_EMBEDDING [a = a]
  |  leftEmbeddingField: "x"
  |  rightEmbeddingField: "none"
  |  |  |
  |  |  COLLSCAN [test.base_coll_reorder_md_base]
  |  |  direction: "forward"
  |  |
  |  COLLSCAN [test.base_coll_reorder_md_a]
  |  direction: "forward"
  |
  COLLSCAN [test.base_coll_reorder_md_b]
  direction: "forward"
```
### Random reordering with seed 5
`"HJ( x = ( COLLSCAN [test.base_coll_reorder_md_a] ), _ = ( NLJ( y = ( COLLSCAN [test.base_coll_reorder_md_b] ), _ = ( COLLSCAN [test.base_coll_reorder_md_base] ) ) ) )"`
```
HASH_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "x"
rightEmbeddingField: "none"
  |  |
  |  NESTED_LOOP_JOIN_EMBEDDING [b = b]
  |  leftEmbeddingField: "y"
  |  rightEmbeddingField: "none"
  |  |  |
  |  |  COLLSCAN [test.base_coll_reorder_md_base]
  |  |  direction: "forward"
  |  |
  |  COLLSCAN [test.base_coll_reorder_md_b]
  |  direction: "forward"
  |
  COLLSCAN [test.base_coll_reorder_md_a]
  direction: "forward"
```
### Random reordering with seed 6
`"HJ( _ = ( NLJ( _ = ( COLLSCAN [test.base_coll_reorder_md_base] ), y = ( COLLSCAN [test.base_coll_reorder_md_b] ) ) ), x = ( COLLSCAN [test.base_coll_reorder_md_a] ) )"`
```
HASH_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.base_coll_reorder_md_a]
  |  direction: "forward"
  |
  NESTED_LOOP_JOIN_EMBEDDING [b = b]
  leftEmbeddingField: "none"
  rightEmbeddingField: "y"
  |  |
  |  COLLSCAN [test.base_coll_reorder_md_b]
  |  direction: "forward"
  |
  COLLSCAN [test.base_coll_reorder_md_base]
  direction: "forward"
```
### Random reordering with seed 8
`"HJ( x = ( COLLSCAN [test.base_coll_reorder_md_a] ), _ = ( NLJ( _ = ( COLLSCAN [test.base_coll_reorder_md_base] ), y = ( COLLSCAN [test.base_coll_reorder_md_b] ) ) ) )"`
```
HASH_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "x"
rightEmbeddingField: "none"
  |  |
  |  NESTED_LOOP_JOIN_EMBEDDING [b = b]
  |  leftEmbeddingField: "none"
  |  rightEmbeddingField: "y"
  |  |  |
  |  |  COLLSCAN [test.base_coll_reorder_md_b]
  |  |  direction: "forward"
  |  |
  |  COLLSCAN [test.base_coll_reorder_md_base]
  |  direction: "forward"
  |
  COLLSCAN [test.base_coll_reorder_md_a]
  direction: "forward"
```
### Random reordering with seed 10
`"HJ( _ = ( NLJ( y = ( COLLSCAN [test.base_coll_reorder_md_b] ), _ = ( COLLSCAN [test.base_coll_reorder_md_base] ) ) ), x = ( COLLSCAN [test.base_coll_reorder_md_a] ) )"`
```
HASH_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.base_coll_reorder_md_a]
  |  direction: "forward"
  |
  NESTED_LOOP_JOIN_EMBEDDING [b = b]
  leftEmbeddingField: "y"
  rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.base_coll_reorder_md_base]
  |  direction: "forward"
  |
  COLLSCAN [test.base_coll_reorder_md_b]
  direction: "forward"
```
### Random reordering with seed 11
`"NLJ( _ = ( HJ( _ = ( COLLSCAN [test.base_coll_reorder_md_base] ), x = ( COLLSCAN [test.base_coll_reorder_md_a] ) ) ), y = ( COLLSCAN [test.base_coll_reorder_md_b] ) )"`
```
NESTED_LOOP_JOIN_EMBEDDING [b = b]
leftEmbeddingField: "none"
rightEmbeddingField: "y"
  |  |
  |  COLLSCAN [test.base_coll_reorder_md_b]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [a = a]
  leftEmbeddingField: "none"
  rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.base_coll_reorder_md_a]
  |  direction: "forward"
  |
  COLLSCAN [test.base_coll_reorder_md_base]
  direction: "forward"
```

## 2. 3-Node graph, base node connected to one node
### No join opt
### Random reordering with seed 0
`"HJ( _ = ( HJ( x = ( COLLSCAN [test.base_coll_reorder_md_a] ), y = ( COLLSCAN [test.base_coll_reorder_md_b] ) ) ), _ = ( COLLSCAN [test.base_coll_reorder_md_base] ) )"`
```
HASH_JOIN_EMBEDDING [x.a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.base_coll_reorder_md_base]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [b = b]
  leftEmbeddingField: "x"
  rightEmbeddingField: "y"
  |  |
  |  COLLSCAN [test.base_coll_reorder_md_b]
  |  direction: "forward"
  |
  COLLSCAN [test.base_coll_reorder_md_a]
  direction: "forward"
```
### Random reordering with seed 1
`"HJ( _ = ( HJ( x = ( COLLSCAN [test.base_coll_reorder_md_a] ), _ = ( COLLSCAN [test.base_coll_reorder_md_base] ) ) ), y = ( COLLSCAN [test.base_coll_reorder_md_b] ) )"`
```
HASH_JOIN_EMBEDDING [x.b = b]
leftEmbeddingField: "none"
rightEmbeddingField: "y"
  |  |
  |  COLLSCAN [test.base_coll_reorder_md_b]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [a = a]
  leftEmbeddingField: "x"
  rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.base_coll_reorder_md_base]
  |  direction: "forward"
  |
  COLLSCAN [test.base_coll_reorder_md_a]
  direction: "forward"
```
### Random reordering with seed 2
`"NLJ( _ = ( NLJ( x = ( COLLSCAN [test.base_coll_reorder_md_a] ), _ = ( COLLSCAN [test.base_coll_reorder_md_base] ) ) ), y = ( COLLSCAN [test.base_coll_reorder_md_b] ) )"`
```
NESTED_LOOP_JOIN_EMBEDDING [x.b = b]
leftEmbeddingField: "none"
rightEmbeddingField: "y"
  |  |
  |  COLLSCAN [test.base_coll_reorder_md_b]
  |  direction: "forward"
  |
  NESTED_LOOP_JOIN_EMBEDDING [a = a]
  leftEmbeddingField: "x"
  rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.base_coll_reorder_md_base]
  |  direction: "forward"
  |
  COLLSCAN [test.base_coll_reorder_md_a]
  direction: "forward"
```
### Random reordering with seed 3
`"HJ( _ = ( COLLSCAN [test.base_coll_reorder_md_base] ), _ = ( NLJ( y = ( COLLSCAN [test.base_coll_reorder_md_b] ), x = ( COLLSCAN [test.base_coll_reorder_md_a] ) ) ) )"`
```
HASH_JOIN_EMBEDDING [a = x.a]
leftEmbeddingField: "none"
rightEmbeddingField: "none"
  |  |
  |  NESTED_LOOP_JOIN_EMBEDDING [b = b]
  |  leftEmbeddingField: "y"
  |  rightEmbeddingField: "x"
  |  |  |
  |  |  COLLSCAN [test.base_coll_reorder_md_a]
  |  |  direction: "forward"
  |  |
  |  COLLSCAN [test.base_coll_reorder_md_b]
  |  direction: "forward"
  |
  COLLSCAN [test.base_coll_reorder_md_base]
  direction: "forward"
```
### Random reordering with seed 4
`"NLJ( _ = ( NLJ( x = ( COLLSCAN [test.base_coll_reorder_md_a] ), y = ( COLLSCAN [test.base_coll_reorder_md_b] ) ) ), _ = ( COLLSCAN [test.base_coll_reorder_md_base] ) )"`
```
NESTED_LOOP_JOIN_EMBEDDING [x.a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.base_coll_reorder_md_base]
  |  direction: "forward"
  |
  NESTED_LOOP_JOIN_EMBEDDING [b = b]
  leftEmbeddingField: "x"
  rightEmbeddingField: "y"
  |  |
  |  COLLSCAN [test.base_coll_reorder_md_b]
  |  direction: "forward"
  |
  COLLSCAN [test.base_coll_reorder_md_a]
  direction: "forward"
```
### Random reordering with seed 6
`"HJ( _ = ( NLJ( x = ( COLLSCAN [test.base_coll_reorder_md_a] ), y = ( COLLSCAN [test.base_coll_reorder_md_b] ) ) ), _ = ( COLLSCAN [test.base_coll_reorder_md_base] ) )"`
```
HASH_JOIN_EMBEDDING [x.a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.base_coll_reorder_md_base]
  |  direction: "forward"
  |
  NESTED_LOOP_JOIN_EMBEDDING [b = b]
  leftEmbeddingField: "x"
  rightEmbeddingField: "y"
  |  |
  |  COLLSCAN [test.base_coll_reorder_md_b]
  |  direction: "forward"
  |
  COLLSCAN [test.base_coll_reorder_md_a]
  direction: "forward"
```
### Random reordering with seed 8
`"HJ( y = ( COLLSCAN [test.base_coll_reorder_md_b] ), _ = ( HJ( _ = ( COLLSCAN [test.base_coll_reorder_md_base] ), x = ( COLLSCAN [test.base_coll_reorder_md_a] ) ) ) )"`
```
HASH_JOIN_EMBEDDING [b = x.b]
leftEmbeddingField: "y"
rightEmbeddingField: "none"
  |  |
  |  HASH_JOIN_EMBEDDING [a = a]
  |  leftEmbeddingField: "none"
  |  rightEmbeddingField: "x"
  |  |  |
  |  |  COLLSCAN [test.base_coll_reorder_md_a]
  |  |  direction: "forward"
  |  |
  |  COLLSCAN [test.base_coll_reorder_md_base]
  |  direction: "forward"
  |
  COLLSCAN [test.base_coll_reorder_md_b]
  direction: "forward"
```
### Random reordering with seed 9
`"NLJ( _ = ( NLJ( y = ( COLLSCAN [test.base_coll_reorder_md_b] ), x = ( COLLSCAN [test.base_coll_reorder_md_a] ) ) ), _ = ( COLLSCAN [test.base_coll_reorder_md_base] ) )"`
```
NESTED_LOOP_JOIN_EMBEDDING [x.a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.base_coll_reorder_md_base]
  |  direction: "forward"
  |
  NESTED_LOOP_JOIN_EMBEDDING [b = b]
  leftEmbeddingField: "y"
  rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.base_coll_reorder_md_a]
  |  direction: "forward"
  |
  COLLSCAN [test.base_coll_reorder_md_b]
  direction: "forward"
```
### Random reordering with seed 10
`"HJ( _ = ( NLJ( y = ( COLLSCAN [test.base_coll_reorder_md_b] ), x = ( COLLSCAN [test.base_coll_reorder_md_a] ) ) ), _ = ( COLLSCAN [test.base_coll_reorder_md_base] ) )"`
```
HASH_JOIN_EMBEDDING [x.a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.base_coll_reorder_md_base]
  |  direction: "forward"
  |
  NESTED_LOOP_JOIN_EMBEDDING [b = b]
  leftEmbeddingField: "y"
  rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.base_coll_reorder_md_a]
  |  direction: "forward"
  |
  COLLSCAN [test.base_coll_reorder_md_b]
  direction: "forward"
```
### Random reordering with seed 11
`"HJ( _ = ( HJ( _ = ( COLLSCAN [test.base_coll_reorder_md_base] ), x = ( COLLSCAN [test.base_coll_reorder_md_a] ) ) ), y = ( COLLSCAN [test.base_coll_reorder_md_b] ) )"`
```
HASH_JOIN_EMBEDDING [x.b = b]
leftEmbeddingField: "none"
rightEmbeddingField: "y"
  |  |
  |  COLLSCAN [test.base_coll_reorder_md_b]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [a = a]
  leftEmbeddingField: "none"
  rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.base_coll_reorder_md_a]
  |  direction: "forward"
  |
  COLLSCAN [test.base_coll_reorder_md_base]
  direction: "forward"
```

## 3. 3-Node graph + potentially inferred edge
### No join opt
### Random reordering with seed 0
`"HJ( _ = ( HJ( y = ( COLLSCAN [test.base_coll_reorder_md_b] ), _ = ( COLLSCAN [test.base_coll_reorder_md_base] ) ) ), x = ( COLLSCAN [test.base_coll_reorder_md_a] ) )"`
```
HASH_JOIN_EMBEDDING [base = base,y.base = base]
leftEmbeddingField: "none"
rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.base_coll_reorder_md_a]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [base = base]
  leftEmbeddingField: "y"
  rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.base_coll_reorder_md_base]
  |  direction: "forward"
  |
  COLLSCAN [test.base_coll_reorder_md_b]
  direction: "forward"
```
### Random reordering with seed 1
`"HJ( _ = ( HJ( x = ( COLLSCAN [test.base_coll_reorder_md_a] ), _ = ( COLLSCAN [test.base_coll_reorder_md_base] ) ) ), y = ( COLLSCAN [test.base_coll_reorder_md_b] ) )"`
```
HASH_JOIN_EMBEDDING [base = base,x.base = base]
leftEmbeddingField: "none"
rightEmbeddingField: "y"
  |  |
  |  COLLSCAN [test.base_coll_reorder_md_b]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [base = base]
  leftEmbeddingField: "x"
  rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.base_coll_reorder_md_base]
  |  direction: "forward"
  |
  COLLSCAN [test.base_coll_reorder_md_a]
  direction: "forward"
```
### Random reordering with seed 2
`"HJ( y = ( COLLSCAN [test.base_coll_reorder_md_b] ), _ = ( HJ( x = ( COLLSCAN [test.base_coll_reorder_md_a] ), _ = ( COLLSCAN [test.base_coll_reorder_md_base] ) ) ) )"`
```
HASH_JOIN_EMBEDDING [base = base,base = x.base]
leftEmbeddingField: "y"
rightEmbeddingField: "none"
  |  |
  |  HASH_JOIN_EMBEDDING [base = base]
  |  leftEmbeddingField: "x"
  |  rightEmbeddingField: "none"
  |  |  |
  |  |  COLLSCAN [test.base_coll_reorder_md_base]
  |  |  direction: "forward"
  |  |
  |  COLLSCAN [test.base_coll_reorder_md_a]
  |  direction: "forward"
  |
  COLLSCAN [test.base_coll_reorder_md_b]
  direction: "forward"
```
### Random reordering with seed 3
`"HJ( _ = ( COLLSCAN [test.base_coll_reorder_md_base] ), _ = ( NLJ( y = ( COLLSCAN [test.base_coll_reorder_md_b] ), x = ( COLLSCAN [test.base_coll_reorder_md_a] ) ) ) )"`
```
HASH_JOIN_EMBEDDING [base = x.base,base = y.base]
leftEmbeddingField: "none"
rightEmbeddingField: "none"
  |  |
  |  NESTED_LOOP_JOIN_EMBEDDING [base = base]
  |  leftEmbeddingField: "y"
  |  rightEmbeddingField: "x"
  |  |  |
  |  |  COLLSCAN [test.base_coll_reorder_md_a]
  |  |  direction: "forward"
  |  |
  |  COLLSCAN [test.base_coll_reorder_md_b]
  |  direction: "forward"
  |
  COLLSCAN [test.base_coll_reorder_md_base]
  direction: "forward"
```
### Random reordering with seed 4
`"NLJ( _ = ( NLJ( x = ( COLLSCAN [test.base_coll_reorder_md_a] ), y = ( COLLSCAN [test.base_coll_reorder_md_b] ) ) ), _ = ( COLLSCAN [test.base_coll_reorder_md_base] ) )"`
```
NESTED_LOOP_JOIN_EMBEDDING [x.base = base,y.base = base]
leftEmbeddingField: "none"
rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.base_coll_reorder_md_base]
  |  direction: "forward"
  |
  NESTED_LOOP_JOIN_EMBEDDING [base = base]
  leftEmbeddingField: "x"
  rightEmbeddingField: "y"
  |  |
  |  COLLSCAN [test.base_coll_reorder_md_b]
  |  direction: "forward"
  |
  COLLSCAN [test.base_coll_reorder_md_a]
  direction: "forward"
```
### Random reordering with seed 5
`"HJ( _ = ( COLLSCAN [test.base_coll_reorder_md_base] ), _ = ( NLJ( x = ( COLLSCAN [test.base_coll_reorder_md_a] ), y = ( COLLSCAN [test.base_coll_reorder_md_b] ) ) ) )"`
```
HASH_JOIN_EMBEDDING [base = x.base,base = y.base]
leftEmbeddingField: "none"
rightEmbeddingField: "none"
  |  |
  |  NESTED_LOOP_JOIN_EMBEDDING [base = base]
  |  leftEmbeddingField: "x"
  |  rightEmbeddingField: "y"
  |  |  |
  |  |  COLLSCAN [test.base_coll_reorder_md_b]
  |  |  direction: "forward"
  |  |
  |  COLLSCAN [test.base_coll_reorder_md_a]
  |  direction: "forward"
  |
  COLLSCAN [test.base_coll_reorder_md_base]
  direction: "forward"
```
### Random reordering with seed 6
`"NLJ( _ = ( HJ( _ = ( COLLSCAN [test.base_coll_reorder_md_base] ), y = ( COLLSCAN [test.base_coll_reorder_md_b] ) ) ), x = ( COLLSCAN [test.base_coll_reorder_md_a] ) )"`
```
NESTED_LOOP_JOIN_EMBEDDING [base = base,y.base = base]
leftEmbeddingField: "none"
rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.base_coll_reorder_md_a]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [base = base]
  leftEmbeddingField: "none"
  rightEmbeddingField: "y"
  |  |
  |  COLLSCAN [test.base_coll_reorder_md_b]
  |  direction: "forward"
  |
  COLLSCAN [test.base_coll_reorder_md_base]
  direction: "forward"
```
### Random reordering with seed 7
`"HJ( _ = ( HJ( _ = ( COLLSCAN [test.base_coll_reorder_md_base] ), y = ( COLLSCAN [test.base_coll_reorder_md_b] ) ) ), x = ( COLLSCAN [test.base_coll_reorder_md_a] ) )"`
```
HASH_JOIN_EMBEDDING [base = base,y.base = base]
leftEmbeddingField: "none"
rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.base_coll_reorder_md_a]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [base = base]
  leftEmbeddingField: "none"
  rightEmbeddingField: "y"
  |  |
  |  COLLSCAN [test.base_coll_reorder_md_b]
  |  direction: "forward"
  |
  COLLSCAN [test.base_coll_reorder_md_base]
  direction: "forward"
```
### Random reordering with seed 8
`"HJ( x = ( COLLSCAN [test.base_coll_reorder_md_a] ), _ = ( NLJ( _ = ( COLLSCAN [test.base_coll_reorder_md_base] ), y = ( COLLSCAN [test.base_coll_reorder_md_b] ) ) ) )"`
```
HASH_JOIN_EMBEDDING [base = base,base = y.base]
leftEmbeddingField: "x"
rightEmbeddingField: "none"
  |  |
  |  NESTED_LOOP_JOIN_EMBEDDING [base = base]
  |  leftEmbeddingField: "none"
  |  rightEmbeddingField: "y"
  |  |  |
  |  |  COLLSCAN [test.base_coll_reorder_md_b]
  |  |  direction: "forward"
  |  |
  |  COLLSCAN [test.base_coll_reorder_md_base]
  |  direction: "forward"
  |
  COLLSCAN [test.base_coll_reorder_md_a]
  direction: "forward"
```
### Random reordering with seed 9
`"NLJ( _ = ( NLJ( y = ( COLLSCAN [test.base_coll_reorder_md_b] ), x = ( COLLSCAN [test.base_coll_reorder_md_a] ) ) ), _ = ( COLLSCAN [test.base_coll_reorder_md_base] ) )"`
```
NESTED_LOOP_JOIN_EMBEDDING [x.base = base,y.base = base]
leftEmbeddingField: "none"
rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.base_coll_reorder_md_base]
  |  direction: "forward"
  |
  NESTED_LOOP_JOIN_EMBEDDING [base = base]
  leftEmbeddingField: "y"
  rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.base_coll_reorder_md_a]
  |  direction: "forward"
  |
  COLLSCAN [test.base_coll_reorder_md_b]
  direction: "forward"
```
### Random reordering with seed 11
`"NLJ( _ = ( HJ( _ = ( COLLSCAN [test.base_coll_reorder_md_base] ), x = ( COLLSCAN [test.base_coll_reorder_md_a] ) ) ), y = ( COLLSCAN [test.base_coll_reorder_md_b] ) )"`
```
NESTED_LOOP_JOIN_EMBEDDING [base = base,x.base = base]
leftEmbeddingField: "none"
rightEmbeddingField: "y"
  |  |
  |  COLLSCAN [test.base_coll_reorder_md_b]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [base = base]
  leftEmbeddingField: "none"
  rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.base_coll_reorder_md_a]
  |  direction: "forward"
  |
  COLLSCAN [test.base_coll_reorder_md_base]
  direction: "forward"
```

## 4. 3-Node graph + intermediate exclusion projection & rename
### No join opt
### Random reordering with seed 0
`"HJ( _ = ( HJ( _ = ( COLLSCAN [test.base_coll_reorder_md_base] ), y = ( COLLSCAN [test.base_coll_reorder_md_b] ) ) ), x = ( COLLSCAN [test.base_coll_reorder_md_a] ) )"`
```
HASH_JOIN_EMBEDDING [m = x]
leftEmbeddingField: "none"
rightEmbeddingField: "x"
  |  |
  |  PROJECTION_DEFAULT
  |  transformBy: { "x" : "$a", "_id" : false }
  |  |
  |  COLLSCAN [test.base_coll_reorder_md_a]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [z = b]
  leftEmbeddingField: "none"
  rightEmbeddingField: "y"
  |  |
  |  PROJECTION_SIMPLE
  |  transformBy: { "_id" : false }
  |  |
  |  COLLSCAN [test.base_coll_reorder_md_b]
  |  direction: "forward"
  |
  PROJECTION_DEFAULT
  transformBy: { "m" : "$a", "z" : "$b", "_id" : false }
  |
  COLLSCAN [test.base_coll_reorder_md_base]
  direction: "forward"
```
### Random reordering with seed 1
`"HJ( _ = ( HJ( _ = ( COLLSCAN [test.base_coll_reorder_md_base] ), x = ( COLLSCAN [test.base_coll_reorder_md_a] ) ) ), y = ( COLLSCAN [test.base_coll_reorder_md_b] ) )"`
```
HASH_JOIN_EMBEDDING [z = b]
leftEmbeddingField: "none"
rightEmbeddingField: "y"
  |  |
  |  PROJECTION_SIMPLE
  |  transformBy: { "_id" : false }
  |  |
  |  COLLSCAN [test.base_coll_reorder_md_b]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [m = x]
  leftEmbeddingField: "none"
  rightEmbeddingField: "x"
  |  |
  |  PROJECTION_DEFAULT
  |  transformBy: { "x" : "$a", "_id" : false }
  |  |
  |  COLLSCAN [test.base_coll_reorder_md_a]
  |  direction: "forward"
  |
  PROJECTION_DEFAULT
  transformBy: { "m" : "$a", "z" : "$b", "_id" : false }
  |
  COLLSCAN [test.base_coll_reorder_md_base]
  direction: "forward"
```
### Random reordering with seed 2
`"HJ( y = ( COLLSCAN [test.base_coll_reorder_md_b] ), _ = ( HJ( x = ( COLLSCAN [test.base_coll_reorder_md_a] ), _ = ( COLLSCAN [test.base_coll_reorder_md_base] ) ) ) )"`
```
HASH_JOIN_EMBEDDING [b = z]
leftEmbeddingField: "y"
rightEmbeddingField: "none"
  |  |
  |  HASH_JOIN_EMBEDDING [x = m]
  |  leftEmbeddingField: "x"
  |  rightEmbeddingField: "none"
  |  |  |
  |  |  PROJECTION_DEFAULT
  |  |  transformBy: { "m" : "$a", "z" : "$b", "_id" : false }
  |  |  |
  |  |  COLLSCAN [test.base_coll_reorder_md_base]
  |  |  direction: "forward"
  |  |
  |  PROJECTION_DEFAULT
  |  transformBy: { "x" : "$a", "_id" : false }
  |  |
  |  COLLSCAN [test.base_coll_reorder_md_a]
  |  direction: "forward"
  |
  PROJECTION_SIMPLE
  transformBy: { "_id" : false }
  |
  COLLSCAN [test.base_coll_reorder_md_b]
  direction: "forward"
```
### Random reordering with seed 3
`"HJ( y = ( COLLSCAN [test.base_coll_reorder_md_b] ), _ = ( NLJ( x = ( COLLSCAN [test.base_coll_reorder_md_a] ), _ = ( COLLSCAN [test.base_coll_reorder_md_base] ) ) ) )"`
```
HASH_JOIN_EMBEDDING [b = z]
leftEmbeddingField: "y"
rightEmbeddingField: "none"
  |  |
  |  NESTED_LOOP_JOIN_EMBEDDING [x = m]
  |  leftEmbeddingField: "x"
  |  rightEmbeddingField: "none"
  |  |  |
  |  |  PROJECTION_DEFAULT
  |  |  transformBy: { "m" : "$a", "z" : "$b", "_id" : false }
  |  |  |
  |  |  COLLSCAN [test.base_coll_reorder_md_base]
  |  |  direction: "forward"
  |  |
  |  PROJECTION_DEFAULT
  |  transformBy: { "x" : "$a", "_id" : false }
  |  |
  |  COLLSCAN [test.base_coll_reorder_md_a]
  |  direction: "forward"
  |
  PROJECTION_SIMPLE
  transformBy: { "_id" : false }
  |
  COLLSCAN [test.base_coll_reorder_md_b]
  direction: "forward"
```
### Random reordering with seed 5
`"HJ( x = ( COLLSCAN [test.base_coll_reorder_md_a] ), _ = ( NLJ( y = ( COLLSCAN [test.base_coll_reorder_md_b] ), _ = ( COLLSCAN [test.base_coll_reorder_md_base] ) ) ) )"`
```
HASH_JOIN_EMBEDDING [x = m]
leftEmbeddingField: "x"
rightEmbeddingField: "none"
  |  |
  |  NESTED_LOOP_JOIN_EMBEDDING [b = z]
  |  leftEmbeddingField: "y"
  |  rightEmbeddingField: "none"
  |  |  |
  |  |  PROJECTION_DEFAULT
  |  |  transformBy: { "m" : "$a", "z" : "$b", "_id" : false }
  |  |  |
  |  |  COLLSCAN [test.base_coll_reorder_md_base]
  |  |  direction: "forward"
  |  |
  |  PROJECTION_SIMPLE
  |  transformBy: { "_id" : false }
  |  |
  |  COLLSCAN [test.base_coll_reorder_md_b]
  |  direction: "forward"
  |
  PROJECTION_DEFAULT
  transformBy: { "x" : "$a", "_id" : false }
  |
  COLLSCAN [test.base_coll_reorder_md_a]
  direction: "forward"
```
### Random reordering with seed 6
`"HJ( _ = ( NLJ( _ = ( COLLSCAN [test.base_coll_reorder_md_base] ), y = ( COLLSCAN [test.base_coll_reorder_md_b] ) ) ), x = ( COLLSCAN [test.base_coll_reorder_md_a] ) )"`
```
HASH_JOIN_EMBEDDING [m = x]
leftEmbeddingField: "none"
rightEmbeddingField: "x"
  |  |
  |  PROJECTION_DEFAULT
  |  transformBy: { "x" : "$a", "_id" : false }
  |  |
  |  COLLSCAN [test.base_coll_reorder_md_a]
  |  direction: "forward"
  |
  NESTED_LOOP_JOIN_EMBEDDING [z = b]
  leftEmbeddingField: "none"
  rightEmbeddingField: "y"
  |  |
  |  PROJECTION_SIMPLE
  |  transformBy: { "_id" : false }
  |  |
  |  COLLSCAN [test.base_coll_reorder_md_b]
  |  direction: "forward"
  |
  PROJECTION_DEFAULT
  transformBy: { "m" : "$a", "z" : "$b", "_id" : false }
  |
  COLLSCAN [test.base_coll_reorder_md_base]
  direction: "forward"
```
### Random reordering with seed 8
`"HJ( x = ( COLLSCAN [test.base_coll_reorder_md_a] ), _ = ( NLJ( _ = ( COLLSCAN [test.base_coll_reorder_md_base] ), y = ( COLLSCAN [test.base_coll_reorder_md_b] ) ) ) )"`
```
HASH_JOIN_EMBEDDING [x = m]
leftEmbeddingField: "x"
rightEmbeddingField: "none"
  |  |
  |  NESTED_LOOP_JOIN_EMBEDDING [z = b]
  |  leftEmbeddingField: "none"
  |  rightEmbeddingField: "y"
  |  |  |
  |  |  PROJECTION_SIMPLE
  |  |  transformBy: { "_id" : false }
  |  |  |
  |  |  COLLSCAN [test.base_coll_reorder_md_b]
  |  |  direction: "forward"
  |  |
  |  PROJECTION_DEFAULT
  |  transformBy: { "m" : "$a", "z" : "$b", "_id" : false }
  |  |
  |  COLLSCAN [test.base_coll_reorder_md_base]
  |  direction: "forward"
  |
  PROJECTION_DEFAULT
  transformBy: { "x" : "$a", "_id" : false }
  |
  COLLSCAN [test.base_coll_reorder_md_a]
  direction: "forward"
```
### Random reordering with seed 10
`"HJ( _ = ( NLJ( y = ( COLLSCAN [test.base_coll_reorder_md_b] ), _ = ( COLLSCAN [test.base_coll_reorder_md_base] ) ) ), x = ( COLLSCAN [test.base_coll_reorder_md_a] ) )"`
```
HASH_JOIN_EMBEDDING [m = x]
leftEmbeddingField: "none"
rightEmbeddingField: "x"
  |  |
  |  PROJECTION_DEFAULT
  |  transformBy: { "x" : "$a", "_id" : false }
  |  |
  |  COLLSCAN [test.base_coll_reorder_md_a]
  |  direction: "forward"
  |
  NESTED_LOOP_JOIN_EMBEDDING [b = z]
  leftEmbeddingField: "y"
  rightEmbeddingField: "none"
  |  |
  |  PROJECTION_DEFAULT
  |  transformBy: { "m" : "$a", "z" : "$b", "_id" : false }
  |  |
  |  COLLSCAN [test.base_coll_reorder_md_base]
  |  direction: "forward"
  |
  PROJECTION_SIMPLE
  transformBy: { "_id" : false }
  |
  COLLSCAN [test.base_coll_reorder_md_b]
  direction: "forward"
```
### Random reordering with seed 11
`"NLJ( _ = ( HJ( _ = ( COLLSCAN [test.base_coll_reorder_md_base] ), x = ( COLLSCAN [test.base_coll_reorder_md_a] ) ) ), y = ( COLLSCAN [test.base_coll_reorder_md_b] ) )"`
```
NESTED_LOOP_JOIN_EMBEDDING [z = b]
leftEmbeddingField: "none"
rightEmbeddingField: "y"
  |  |
  |  PROJECTION_SIMPLE
  |  transformBy: { "_id" : false }
  |  |
  |  COLLSCAN [test.base_coll_reorder_md_b]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [m = x]
  leftEmbeddingField: "none"
  rightEmbeddingField: "x"
  |  |
  |  PROJECTION_DEFAULT
  |  transformBy: { "x" : "$a", "_id" : false }
  |  |
  |  COLLSCAN [test.base_coll_reorder_md_a]
  |  direction: "forward"
  |
  PROJECTION_DEFAULT
  transformBy: { "m" : "$a", "z" : "$b", "_id" : false }
  |
  COLLSCAN [test.base_coll_reorder_md_base]
  direction: "forward"
```

## 5. 4-Node graph + potentially inferred edges & filters
### No join opt
### Random reordering with seed 0
`"NLJ( _ = ( HJ( _ = ( HJ( _ = ( COLLSCAN [test.base_coll_reorder_md_base] ), y = ( COLLSCAN [test.base_coll_reorder_md_b] ) ) ), z = ( COLLSCAN [test.base_coll_reorder_md_base] ) ) ), x = ( COLLSCAN [test.base_coll_reorder_md_a] ) )"`
```
NESTED_LOOP_JOIN_EMBEDDING [base = base,y.base = base,z.base = base]
leftEmbeddingField: "none"
rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.base_coll_reorder_md_a]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [y.base = base,base = base]
  leftEmbeddingField: "none"
  rightEmbeddingField: "z"
  |  |
  |  COLLSCAN [test.base_coll_reorder_md_base]
  |  filter: { "base" : { "$gt" : 3 } }
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [base = base]
  leftEmbeddingField: "none"
  rightEmbeddingField: "y"
  |  |
  |  COLLSCAN [test.base_coll_reorder_md_b]
  |  direction: "forward"
  |
  COLLSCAN [test.base_coll_reorder_md_base]
  filter: { "b" : { "$eq" : 3 } }
  direction: "forward"
```
### Random reordering with seed 1
`"HJ( y = ( COLLSCAN [test.base_coll_reorder_md_b] ), _ = ( NLJ( _ = ( HJ( x = ( COLLSCAN [test.base_coll_reorder_md_a] ), _ = ( COLLSCAN [test.base_coll_reorder_md_base] ) ) ), z = ( COLLSCAN [test.base_coll_reorder_md_base] ) ) ) )"`
```
HASH_JOIN_EMBEDDING [base = base,base = z.base,base = x.base]
leftEmbeddingField: "y"
rightEmbeddingField: "none"
  |  |
  |  NESTED_LOOP_JOIN_EMBEDDING [base = base,x.base = base]
  |  leftEmbeddingField: "none"
  |  rightEmbeddingField: "z"
  |  |  |
  |  |  COLLSCAN [test.base_coll_reorder_md_base]
  |  |  filter: { "base" : { "$gt" : 3 } }
  |  |  direction: "forward"
  |  |
  |  HASH_JOIN_EMBEDDING [base = base]
  |  leftEmbeddingField: "x"
  |  rightEmbeddingField: "none"
  |  |  |
  |  |  COLLSCAN [test.base_coll_reorder_md_base]
  |  |  filter: { "b" : { "$eq" : 3 } }
  |  |  direction: "forward"
  |  |
  |  COLLSCAN [test.base_coll_reorder_md_a]
  |  direction: "forward"
  |
  COLLSCAN [test.base_coll_reorder_md_b]
  direction: "forward"
```
### Random reordering with seed 2
`"NLJ( _ = ( HJ( z = ( COLLSCAN [test.base_coll_reorder_md_base] ), _ = ( HJ( y = ( COLLSCAN [test.base_coll_reorder_md_b] ), _ = ( COLLSCAN [test.base_coll_reorder_md_base] ) ) ) ) ), x = ( COLLSCAN [test.base_coll_reorder_md_a] ) )"`
```
NESTED_LOOP_JOIN_EMBEDDING [base = base,y.base = base,z.base = base]
leftEmbeddingField: "none"
rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.base_coll_reorder_md_a]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [base = y.base,base = base]
  leftEmbeddingField: "z"
  rightEmbeddingField: "none"
  |  |
  |  HASH_JOIN_EMBEDDING [base = base]
  |  leftEmbeddingField: "y"
  |  rightEmbeddingField: "none"
  |  |  |
  |  |  COLLSCAN [test.base_coll_reorder_md_base]
  |  |  filter: { "b" : { "$eq" : 3 } }
  |  |  direction: "forward"
  |  |
  |  COLLSCAN [test.base_coll_reorder_md_b]
  |  direction: "forward"
  |
  COLLSCAN [test.base_coll_reorder_md_base]
  filter: { "base" : { "$gt" : 3 } }
  direction: "forward"
```
### Random reordering with seed 3
`"HJ( z = ( COLLSCAN [test.base_coll_reorder_md_base] ), _ = ( HJ( _ = ( HJ( _ = ( COLLSCAN [test.base_coll_reorder_md_base] ), y = ( COLLSCAN [test.base_coll_reorder_md_b] ) ) ), x = ( COLLSCAN [test.base_coll_reorder_md_a] ) ) ) )"`
```
HASH_JOIN_EMBEDDING [base = y.base,base = base,base = x.base]
leftEmbeddingField: "z"
rightEmbeddingField: "none"
  |  |
  |  HASH_JOIN_EMBEDDING [base = base,y.base = base]
  |  leftEmbeddingField: "none"
  |  rightEmbeddingField: "x"
  |  |  |
  |  |  COLLSCAN [test.base_coll_reorder_md_a]
  |  |  direction: "forward"
  |  |
  |  HASH_JOIN_EMBEDDING [base = base]
  |  leftEmbeddingField: "none"
  |  rightEmbeddingField: "y"
  |  |  |
  |  |  COLLSCAN [test.base_coll_reorder_md_b]
  |  |  direction: "forward"
  |  |
  |  COLLSCAN [test.base_coll_reorder_md_base]
  |  filter: { "b" : { "$eq" : 3 } }
  |  direction: "forward"
  |
  COLLSCAN [test.base_coll_reorder_md_base]
  filter: { "base" : { "$gt" : 3 } }
  direction: "forward"
```
### Random reordering with seed 4
`"NLJ( _ = ( HJ( _ = ( COLLSCAN [test.base_coll_reorder_md_base] ), _ = ( NLJ( y = ( COLLSCAN [test.base_coll_reorder_md_b] ), x = ( COLLSCAN [test.base_coll_reorder_md_a] ) ) ) ) ), z = ( COLLSCAN [test.base_coll_reorder_md_base] ) )"`
```
NESTED_LOOP_JOIN_EMBEDDING [y.base = base,base = base,x.base = base]
leftEmbeddingField: "none"
rightEmbeddingField: "z"
  |  |
  |  COLLSCAN [test.base_coll_reorder_md_base]
  |  filter: { "base" : { "$gt" : 3 } }
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [base = x.base,base = y.base]
  leftEmbeddingField: "none"
  rightEmbeddingField: "none"
  |  |
  |  NESTED_LOOP_JOIN_EMBEDDING [base = base]
  |  leftEmbeddingField: "y"
  |  rightEmbeddingField: "x"
  |  |  |
  |  |  COLLSCAN [test.base_coll_reorder_md_a]
  |  |  direction: "forward"
  |  |
  |  COLLSCAN [test.base_coll_reorder_md_b]
  |  direction: "forward"
  |
  COLLSCAN [test.base_coll_reorder_md_base]
  filter: { "b" : { "$eq" : 3 } }
  direction: "forward"
```
### Random reordering with seed 5
`"HJ( y = ( COLLSCAN [test.base_coll_reorder_md_b] ), _ = ( HJ( _ = ( COLLSCAN [test.base_coll_reorder_md_base] ), _ = ( HJ( x = ( COLLSCAN [test.base_coll_reorder_md_a] ), z = ( COLLSCAN [test.base_coll_reorder_md_base] ) ) ) ) ) )"`
```
HASH_JOIN_EMBEDDING [base = base,base = z.base,base = x.base]
leftEmbeddingField: "y"
rightEmbeddingField: "none"
  |  |
  |  HASH_JOIN_EMBEDDING [base = x.base,base = z.base]
  |  leftEmbeddingField: "none"
  |  rightEmbeddingField: "none"
  |  |  |
  |  |  HASH_JOIN_EMBEDDING [base = base]
  |  |  leftEmbeddingField: "x"
  |  |  rightEmbeddingField: "z"
  |  |  |  |
  |  |  |  COLLSCAN [test.base_coll_reorder_md_base]
  |  |  |  filter: { "base" : { "$gt" : 3 } }
  |  |  |  direction: "forward"
  |  |  |
  |  |  COLLSCAN [test.base_coll_reorder_md_a]
  |  |  direction: "forward"
  |  |
  |  COLLSCAN [test.base_coll_reorder_md_base]
  |  filter: { "b" : { "$eq" : 3 } }
  |  direction: "forward"
  |
  COLLSCAN [test.base_coll_reorder_md_b]
  direction: "forward"
```
### Random reordering with seed 6
`"HJ( _ = ( COLLSCAN [test.base_coll_reorder_md_base] ), _ = ( HJ( z = ( COLLSCAN [test.base_coll_reorder_md_base] ), _ = ( NLJ( y = ( COLLSCAN [test.base_coll_reorder_md_b] ), x = ( COLLSCAN [test.base_coll_reorder_md_a] ) ) ) ) ) )"`
```
HASH_JOIN_EMBEDDING [base = x.base,base = y.base,base = z.base]
leftEmbeddingField: "none"
rightEmbeddingField: "none"
  |  |
  |  HASH_JOIN_EMBEDDING [base = y.base,base = x.base]
  |  leftEmbeddingField: "z"
  |  rightEmbeddingField: "none"
  |  |  |
  |  |  NESTED_LOOP_JOIN_EMBEDDING [base = base]
  |  |  leftEmbeddingField: "y"
  |  |  rightEmbeddingField: "x"
  |  |  |  |
  |  |  |  COLLSCAN [test.base_coll_reorder_md_a]
  |  |  |  direction: "forward"
  |  |  |
  |  |  COLLSCAN [test.base_coll_reorder_md_b]
  |  |  direction: "forward"
  |  |
  |  COLLSCAN [test.base_coll_reorder_md_base]
  |  filter: { "base" : { "$gt" : 3 } }
  |  direction: "forward"
  |
  COLLSCAN [test.base_coll_reorder_md_base]
  filter: { "b" : { "$eq" : 3 } }
  direction: "forward"
```
### Random reordering with seed 7
`"NLJ( _ = ( HJ( x = ( COLLSCAN [test.base_coll_reorder_md_a] ), _ = ( HJ( z = ( COLLSCAN [test.base_coll_reorder_md_base] ), y = ( COLLSCAN [test.base_coll_reorder_md_b] ) ) ) ) ), _ = ( COLLSCAN [test.base_coll_reorder_md_base] ) )"`
```
NESTED_LOOP_JOIN_EMBEDDING [x.base = base,y.base = base,z.base = base]
leftEmbeddingField: "none"
rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.base_coll_reorder_md_base]
  |  filter: { "b" : { "$eq" : 3 } }
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [base = y.base,base = z.base]
  leftEmbeddingField: "x"
  rightEmbeddingField: "none"
  |  |
  |  HASH_JOIN_EMBEDDING [base = base]
  |  leftEmbeddingField: "z"
  |  rightEmbeddingField: "y"
  |  |  |
  |  |  COLLSCAN [test.base_coll_reorder_md_b]
  |  |  direction: "forward"
  |  |
  |  COLLSCAN [test.base_coll_reorder_md_base]
  |  filter: { "base" : { "$gt" : 3 } }
  |  direction: "forward"
  |
  COLLSCAN [test.base_coll_reorder_md_a]
  direction: "forward"
```
### Random reordering with seed 8
`"NLJ( _ = ( HJ( _ = ( COLLSCAN [test.base_coll_reorder_md_base] ), _ = ( HJ( y = ( COLLSCAN [test.base_coll_reorder_md_b] ), z = ( COLLSCAN [test.base_coll_reorder_md_base] ) ) ) ) ), x = ( COLLSCAN [test.base_coll_reorder_md_a] ) )"`
```
NESTED_LOOP_JOIN_EMBEDDING [base = base,y.base = base,z.base = base]
leftEmbeddingField: "none"
rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.base_coll_reorder_md_a]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [base = y.base,base = z.base]
  leftEmbeddingField: "none"
  rightEmbeddingField: "none"
  |  |
  |  HASH_JOIN_EMBEDDING [base = base]
  |  leftEmbeddingField: "y"
  |  rightEmbeddingField: "z"
  |  |  |
  |  |  COLLSCAN [test.base_coll_reorder_md_base]
  |  |  filter: { "base" : { "$gt" : 3 } }
  |  |  direction: "forward"
  |  |
  |  COLLSCAN [test.base_coll_reorder_md_b]
  |  direction: "forward"
  |
  COLLSCAN [test.base_coll_reorder_md_base]
  filter: { "b" : { "$eq" : 3 } }
  direction: "forward"
```
### Random reordering with seed 9
`"HJ( x = ( COLLSCAN [test.base_coll_reorder_md_a] ), _ = ( HJ( _ = ( NLJ( y = ( COLLSCAN [test.base_coll_reorder_md_b] ), z = ( COLLSCAN [test.base_coll_reorder_md_base] ) ) ), _ = ( COLLSCAN [test.base_coll_reorder_md_base] ) ) ) )"`
```
HASH_JOIN_EMBEDDING [base = base,base = y.base,base = z.base]
leftEmbeddingField: "x"
rightEmbeddingField: "none"
  |  |
  |  HASH_JOIN_EMBEDDING [y.base = base,z.base = base]
  |  leftEmbeddingField: "none"
  |  rightEmbeddingField: "none"
  |  |  |
  |  |  COLLSCAN [test.base_coll_reorder_md_base]
  |  |  filter: { "b" : { "$eq" : 3 } }
  |  |  direction: "forward"
  |  |
  |  NESTED_LOOP_JOIN_EMBEDDING [base = base]
  |  leftEmbeddingField: "y"
  |  rightEmbeddingField: "z"
  |  |  |
  |  |  COLLSCAN [test.base_coll_reorder_md_base]
  |  |  filter: { "base" : { "$gt" : 3 } }
  |  |  direction: "forward"
  |  |
  |  COLLSCAN [test.base_coll_reorder_md_b]
  |  direction: "forward"
  |
  COLLSCAN [test.base_coll_reorder_md_a]
  direction: "forward"
```
### Random reordering with seed 10
`"HJ( z = ( COLLSCAN [test.base_coll_reorder_md_base] ), _ = ( HJ( _ = ( NLJ( x = ( COLLSCAN [test.base_coll_reorder_md_a] ), y = ( COLLSCAN [test.base_coll_reorder_md_b] ) ) ), _ = ( COLLSCAN [test.base_coll_reorder_md_base] ) ) ) )"`
```
HASH_JOIN_EMBEDDING [base = y.base,base = base,base = x.base]
leftEmbeddingField: "z"
rightEmbeddingField: "none"
  |  |
  |  HASH_JOIN_EMBEDDING [x.base = base,y.base = base]
  |  leftEmbeddingField: "none"
  |  rightEmbeddingField: "none"
  |  |  |
  |  |  COLLSCAN [test.base_coll_reorder_md_base]
  |  |  filter: { "b" : { "$eq" : 3 } }
  |  |  direction: "forward"
  |  |
  |  NESTED_LOOP_JOIN_EMBEDDING [base = base]
  |  leftEmbeddingField: "x"
  |  rightEmbeddingField: "y"
  |  |  |
  |  |  COLLSCAN [test.base_coll_reorder_md_b]
  |  |  direction: "forward"
  |  |
  |  COLLSCAN [test.base_coll_reorder_md_a]
  |  direction: "forward"
  |
  COLLSCAN [test.base_coll_reorder_md_base]
  filter: { "base" : { "$gt" : 3 } }
  direction: "forward"
```

## 6. 5-Node graph + filters
### No join opt
### Random reordering with seed 0
`"NLJ( _ = ( HJ( _ = ( HJ( _ = ( HJ( ddd = ( COLLSCAN [test.base_coll_reorder_md_b] ), _ = ( COLLSCAN [test.base_coll_reorder_md_base] ) ) ), aaa = ( COLLSCAN [test.base_coll_reorder_md_a] ) ) ), ccc = ( COLLSCAN [test.base_coll_reorder_md_base] ) ) ), bbb = ( COLLSCAN [test.base_coll_reorder_md_b] ) )"`
```
NESTED_LOOP_JOIN_EMBEDDING [b = b]
leftEmbeddingField: "none"
rightEmbeddingField: "bbb"
  |  |
  |  COLLSCAN [test.base_coll_reorder_md_b]
  |  filter: { "$and" : [ { "b" : { "$eq" : 3 } }, { "base" : { "$gt" : 20 } } ] }
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [aaa.base = base]
  leftEmbeddingField: "none"
  rightEmbeddingField: "ccc"
  |  |
  |  COLLSCAN [test.base_coll_reorder_md_base]
  |  filter: { "$and" : [ { "b" : { "$lt" : 0 } }, { "base" : { "$in" : [ 22, 33 ] } } ] }
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [a = a]
  leftEmbeddingField: "none"
  rightEmbeddingField: "aaa"
  |  |
  |  COLLSCAN [test.base_coll_reorder_md_a]
  |  filter: { "base" : { "$in" : [ 22, 33 ] } }
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [base = base]
  leftEmbeddingField: "ddd"
  rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.base_coll_reorder_md_base]
  |  filter: { "b" : { "$eq" : 3 } }
  |  direction: "forward"
  |
  COLLSCAN [test.base_coll_reorder_md_b]
  filter: { "b" : { "$gt" : 0 } }
  direction: "forward"
```
### Random reordering with seed 1
`"HJ( ddd = ( COLLSCAN [test.base_coll_reorder_md_b] ), _ = ( HJ( bbb = ( COLLSCAN [test.base_coll_reorder_md_b] ), _ = ( HJ( _ = ( HJ( _ = ( COLLSCAN [test.base_coll_reorder_md_base] ), aaa = ( COLLSCAN [test.base_coll_reorder_md_a] ) ) ), ccc = ( COLLSCAN [test.base_coll_reorder_md_base] ) ) ) ) ) )"`
```
HASH_JOIN_EMBEDDING [base = base]
leftEmbeddingField: "ddd"
rightEmbeddingField: "none"
  |  |
  |  HASH_JOIN_EMBEDDING [b = b]
  |  leftEmbeddingField: "bbb"
  |  rightEmbeddingField: "none"
  |  |  |
  |  |  HASH_JOIN_EMBEDDING [aaa.base = base]
  |  |  leftEmbeddingField: "none"
  |  |  rightEmbeddingField: "ccc"
  |  |  |  |
  |  |  |  COLLSCAN [test.base_coll_reorder_md_base]
  |  |  |  filter: { "$and" : [ { "b" : { "$lt" : 0 } }, { "base" : { "$in" : [ 22, 33 ] } } ] }
  |  |  |  direction: "forward"
  |  |  |
  |  |  HASH_JOIN_EMBEDDING [a = a]
  |  |  leftEmbeddingField: "none"
  |  |  rightEmbeddingField: "aaa"
  |  |  |  |
  |  |  |  COLLSCAN [test.base_coll_reorder_md_a]
  |  |  |  filter: { "base" : { "$in" : [ 22, 33 ] } }
  |  |  |  direction: "forward"
  |  |  |
  |  |  COLLSCAN [test.base_coll_reorder_md_base]
  |  |  filter: { "b" : { "$eq" : 3 } }
  |  |  direction: "forward"
  |  |
  |  COLLSCAN [test.base_coll_reorder_md_b]
  |  filter: { "$and" : [ { "b" : { "$eq" : 3 } }, { "base" : { "$gt" : 20 } } ] }
  |  direction: "forward"
  |
  COLLSCAN [test.base_coll_reorder_md_b]
  filter: { "b" : { "$gt" : 0 } }
  direction: "forward"
```
### Random reordering with seed 2
`"NLJ( _ = ( NLJ( _ = ( HJ( _ = ( COLLSCAN [test.base_coll_reorder_md_base] ), _ = ( HJ( aaa = ( COLLSCAN [test.base_coll_reorder_md_a] ), ccc = ( COLLSCAN [test.base_coll_reorder_md_base] ) ) ) ) ), bbb = ( COLLSCAN [test.base_coll_reorder_md_b] ) ) ), ddd = ( COLLSCAN [test.base_coll_reorder_md_b] ) )"`
```
NESTED_LOOP_JOIN_EMBEDDING [base = base]
leftEmbeddingField: "none"
rightEmbeddingField: "ddd"
  |  |
  |  COLLSCAN [test.base_coll_reorder_md_b]
  |  filter: { "b" : { "$gt" : 0 } }
  |  direction: "forward"
  |
  NESTED_LOOP_JOIN_EMBEDDING [b = b]
  leftEmbeddingField: "none"
  rightEmbeddingField: "bbb"
  |  |
  |  COLLSCAN [test.base_coll_reorder_md_b]
  |  filter: { "$and" : [ { "b" : { "$eq" : 3 } }, { "base" : { "$gt" : 20 } } ] }
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [a = aaa.a]
  leftEmbeddingField: "none"
  rightEmbeddingField: "none"
  |  |
  |  HASH_JOIN_EMBEDDING [base = base]
  |  leftEmbeddingField: "aaa"
  |  rightEmbeddingField: "ccc"
  |  |  |
  |  |  COLLSCAN [test.base_coll_reorder_md_base]
  |  |  filter: { "$and" : [ { "b" : { "$lt" : 0 } }, { "base" : { "$in" : [ 22, 33 ] } } ] }
  |  |  direction: "forward"
  |  |
  |  COLLSCAN [test.base_coll_reorder_md_a]
  |  filter: { "base" : { "$in" : [ 22, 33 ] } }
  |  direction: "forward"
  |
  COLLSCAN [test.base_coll_reorder_md_base]
  filter: { "b" : { "$eq" : 3 } }
  direction: "forward"
```
### Random reordering with seed 3
`"HJ( _ = ( NLJ( _ = ( HJ( _ = ( NLJ( ccc = ( COLLSCAN [test.base_coll_reorder_md_base] ), aaa = ( COLLSCAN [test.base_coll_reorder_md_a] ) ) ), _ = ( COLLSCAN [test.base_coll_reorder_md_base] ) ) ), bbb = ( COLLSCAN [test.base_coll_reorder_md_b] ) ) ), ddd = ( COLLSCAN [test.base_coll_reorder_md_b] ) )"`
```
HASH_JOIN_EMBEDDING [base = base]
leftEmbeddingField: "none"
rightEmbeddingField: "ddd"
  |  |
  |  COLLSCAN [test.base_coll_reorder_md_b]
  |  filter: { "b" : { "$gt" : 0 } }
  |  direction: "forward"
  |
  NESTED_LOOP_JOIN_EMBEDDING [b = b]
  leftEmbeddingField: "none"
  rightEmbeddingField: "bbb"
  |  |
  |  COLLSCAN [test.base_coll_reorder_md_b]
  |  filter: { "$and" : [ { "b" : { "$eq" : 3 } }, { "base" : { "$gt" : 20 } } ] }
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [aaa.a = a]
  leftEmbeddingField: "none"
  rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.base_coll_reorder_md_base]
  |  filter: { "b" : { "$eq" : 3 } }
  |  direction: "forward"
  |
  NESTED_LOOP_JOIN_EMBEDDING [base = base]
  leftEmbeddingField: "ccc"
  rightEmbeddingField: "aaa"
  |  |
  |  COLLSCAN [test.base_coll_reorder_md_a]
  |  filter: { "base" : { "$in" : [ 22, 33 ] } }
  |  direction: "forward"
  |
  COLLSCAN [test.base_coll_reorder_md_base]
  filter: { "$and" : [ { "b" : { "$lt" : 0 } }, { "base" : { "$in" : [ 22, 33 ] } } ] }
  direction: "forward"
```
### Random reordering with seed 4
`"NLJ( _ = ( HJ( ccc = ( COLLSCAN [test.base_coll_reorder_md_base] ), _ = ( HJ( aaa = ( COLLSCAN [test.base_coll_reorder_md_a] ), _ = ( HJ( bbb = ( COLLSCAN [test.base_coll_reorder_md_b] ), _ = ( COLLSCAN [test.base_coll_reorder_md_base] ) ) ) ) ) ) ), ddd = ( COLLSCAN [test.base_coll_reorder_md_b] ) )"`
```
NESTED_LOOP_JOIN_EMBEDDING [base = base]
leftEmbeddingField: "none"
rightEmbeddingField: "ddd"
  |  |
  |  COLLSCAN [test.base_coll_reorder_md_b]
  |  filter: { "b" : { "$gt" : 0 } }
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [base = aaa.base]
  leftEmbeddingField: "ccc"
  rightEmbeddingField: "none"
  |  |
  |  HASH_JOIN_EMBEDDING [a = a]
  |  leftEmbeddingField: "aaa"
  |  rightEmbeddingField: "none"
  |  |  |
  |  |  HASH_JOIN_EMBEDDING [b = b]
  |  |  leftEmbeddingField: "bbb"
  |  |  rightEmbeddingField: "none"
  |  |  |  |
  |  |  |  COLLSCAN [test.base_coll_reorder_md_base]
  |  |  |  filter: { "b" : { "$eq" : 3 } }
  |  |  |  direction: "forward"
  |  |  |
  |  |  COLLSCAN [test.base_coll_reorder_md_b]
  |  |  filter: { "$and" : [ { "b" : { "$eq" : 3 } }, { "base" : { "$gt" : 20 } } ] }
  |  |  direction: "forward"
  |  |
  |  COLLSCAN [test.base_coll_reorder_md_a]
  |  filter: { "base" : { "$in" : [ 22, 33 ] } }
  |  direction: "forward"
  |
  COLLSCAN [test.base_coll_reorder_md_base]
  filter: { "$and" : [ { "b" : { "$lt" : 0 } }, { "base" : { "$in" : [ 22, 33 ] } } ] }
  direction: "forward"
```
### Random reordering with seed 5
`"NLJ( _ = ( HJ( bbb = ( COLLSCAN [test.base_coll_reorder_md_b] ), _ = ( NLJ( _ = ( NLJ( ccc = ( COLLSCAN [test.base_coll_reorder_md_base] ), aaa = ( COLLSCAN [test.base_coll_reorder_md_a] ) ) ), _ = ( COLLSCAN [test.base_coll_reorder_md_base] ) ) ) ) ), ddd = ( COLLSCAN [test.base_coll_reorder_md_b] ) )"`
```
NESTED_LOOP_JOIN_EMBEDDING [base = base]
leftEmbeddingField: "none"
rightEmbeddingField: "ddd"
  |  |
  |  COLLSCAN [test.base_coll_reorder_md_b]
  |  filter: { "b" : { "$gt" : 0 } }
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [b = b]
  leftEmbeddingField: "bbb"
  rightEmbeddingField: "none"
  |  |
  |  NESTED_LOOP_JOIN_EMBEDDING [aaa.a = a]
  |  leftEmbeddingField: "none"
  |  rightEmbeddingField: "none"
  |  |  |
  |  |  COLLSCAN [test.base_coll_reorder_md_base]
  |  |  filter: { "b" : { "$eq" : 3 } }
  |  |  direction: "forward"
  |  |
  |  NESTED_LOOP_JOIN_EMBEDDING [base = base]
  |  leftEmbeddingField: "ccc"
  |  rightEmbeddingField: "aaa"
  |  |  |
  |  |  COLLSCAN [test.base_coll_reorder_md_a]
  |  |  filter: { "base" : { "$in" : [ 22, 33 ] } }
  |  |  direction: "forward"
  |  |
  |  COLLSCAN [test.base_coll_reorder_md_base]
  |  filter: { "$and" : [ { "b" : { "$lt" : 0 } }, { "base" : { "$in" : [ 22, 33 ] } } ] }
  |  direction: "forward"
  |
  COLLSCAN [test.base_coll_reorder_md_b]
  filter: { "$and" : [ { "b" : { "$eq" : 3 } }, { "base" : { "$gt" : 20 } } ] }
  direction: "forward"
```
### Random reordering with seed 6
`"HJ( ddd = ( COLLSCAN [test.base_coll_reorder_md_b] ), _ = ( HJ( ccc = ( COLLSCAN [test.base_coll_reorder_md_base] ), _ = ( HJ( aaa = ( COLLSCAN [test.base_coll_reorder_md_a] ), _ = ( HJ( _ = ( COLLSCAN [test.base_coll_reorder_md_base] ), bbb = ( COLLSCAN [test.base_coll_reorder_md_b] ) ) ) ) ) ) ) )"`
```
HASH_JOIN_EMBEDDING [base = base]
leftEmbeddingField: "ddd"
rightEmbeddingField: "none"
  |  |
  |  HASH_JOIN_EMBEDDING [base = aaa.base]
  |  leftEmbeddingField: "ccc"
  |  rightEmbeddingField: "none"
  |  |  |
  |  |  HASH_JOIN_EMBEDDING [a = a]
  |  |  leftEmbeddingField: "aaa"
  |  |  rightEmbeddingField: "none"
  |  |  |  |
  |  |  |  HASH_JOIN_EMBEDDING [b = b]
  |  |  |  leftEmbeddingField: "none"
  |  |  |  rightEmbeddingField: "bbb"
  |  |  |  |  |
  |  |  |  |  COLLSCAN [test.base_coll_reorder_md_b]
  |  |  |  |  filter: { "$and" : [ { "b" : { "$eq" : 3 } }, { "base" : { "$gt" : 20 } } ] }
  |  |  |  |  direction: "forward"
  |  |  |  |
  |  |  |  COLLSCAN [test.base_coll_reorder_md_base]
  |  |  |  filter: { "b" : { "$eq" : 3 } }
  |  |  |  direction: "forward"
  |  |  |
  |  |  COLLSCAN [test.base_coll_reorder_md_a]
  |  |  filter: { "base" : { "$in" : [ 22, 33 ] } }
  |  |  direction: "forward"
  |  |
  |  COLLSCAN [test.base_coll_reorder_md_base]
  |  filter: { "$and" : [ { "b" : { "$lt" : 0 } }, { "base" : { "$in" : [ 22, 33 ] } } ] }
  |  direction: "forward"
  |
  COLLSCAN [test.base_coll_reorder_md_b]
  filter: { "b" : { "$gt" : 0 } }
  direction: "forward"
```
### Random reordering with seed 7
`"NLJ( _ = ( NLJ( _ = ( NLJ( _ = ( HJ( _ = ( COLLSCAN [test.base_coll_reorder_md_base] ), ddd = ( COLLSCAN [test.base_coll_reorder_md_b] ) ) ), aaa = ( COLLSCAN [test.base_coll_reorder_md_a] ) ) ), ccc = ( COLLSCAN [test.base_coll_reorder_md_base] ) ) ), bbb = ( COLLSCAN [test.base_coll_reorder_md_b] ) )"`
```
NESTED_LOOP_JOIN_EMBEDDING [b = b]
leftEmbeddingField: "none"
rightEmbeddingField: "bbb"
  |  |
  |  COLLSCAN [test.base_coll_reorder_md_b]
  |  filter: { "$and" : [ { "b" : { "$eq" : 3 } }, { "base" : { "$gt" : 20 } } ] }
  |  direction: "forward"
  |
  NESTED_LOOP_JOIN_EMBEDDING [aaa.base = base]
  leftEmbeddingField: "none"
  rightEmbeddingField: "ccc"
  |  |
  |  COLLSCAN [test.base_coll_reorder_md_base]
  |  filter: { "$and" : [ { "b" : { "$lt" : 0 } }, { "base" : { "$in" : [ 22, 33 ] } } ] }
  |  direction: "forward"
  |
  NESTED_LOOP_JOIN_EMBEDDING [a = a]
  leftEmbeddingField: "none"
  rightEmbeddingField: "aaa"
  |  |
  |  COLLSCAN [test.base_coll_reorder_md_a]
  |  filter: { "base" : { "$in" : [ 22, 33 ] } }
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [base = base]
  leftEmbeddingField: "none"
  rightEmbeddingField: "ddd"
  |  |
  |  COLLSCAN [test.base_coll_reorder_md_b]
  |  filter: { "b" : { "$gt" : 0 } }
  |  direction: "forward"
  |
  COLLSCAN [test.base_coll_reorder_md_base]
  filter: { "b" : { "$eq" : 3 } }
  direction: "forward"
```
### Random reordering with seed 8
`"NLJ( _ = ( NLJ( _ = ( HJ( _ = ( COLLSCAN [test.base_coll_reorder_md_base] ), _ = ( NLJ( ccc = ( COLLSCAN [test.base_coll_reorder_md_base] ), aaa = ( COLLSCAN [test.base_coll_reorder_md_a] ) ) ) ) ), ddd = ( COLLSCAN [test.base_coll_reorder_md_b] ) ) ), bbb = ( COLLSCAN [test.base_coll_reorder_md_b] ) )"`
```
NESTED_LOOP_JOIN_EMBEDDING [b = b]
leftEmbeddingField: "none"
rightEmbeddingField: "bbb"
  |  |
  |  COLLSCAN [test.base_coll_reorder_md_b]
  |  filter: { "$and" : [ { "b" : { "$eq" : 3 } }, { "base" : { "$gt" : 20 } } ] }
  |  direction: "forward"
  |
  NESTED_LOOP_JOIN_EMBEDDING [base = base]
  leftEmbeddingField: "none"
  rightEmbeddingField: "ddd"
  |  |
  |  COLLSCAN [test.base_coll_reorder_md_b]
  |  filter: { "b" : { "$gt" : 0 } }
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [a = aaa.a]
  leftEmbeddingField: "none"
  rightEmbeddingField: "none"
  |  |
  |  NESTED_LOOP_JOIN_EMBEDDING [base = base]
  |  leftEmbeddingField: "ccc"
  |  rightEmbeddingField: "aaa"
  |  |  |
  |  |  COLLSCAN [test.base_coll_reorder_md_a]
  |  |  filter: { "base" : { "$in" : [ 22, 33 ] } }
  |  |  direction: "forward"
  |  |
  |  COLLSCAN [test.base_coll_reorder_md_base]
  |  filter: { "$and" : [ { "b" : { "$lt" : 0 } }, { "base" : { "$in" : [ 22, 33 ] } } ] }
  |  direction: "forward"
  |
  COLLSCAN [test.base_coll_reorder_md_base]
  filter: { "b" : { "$eq" : 3 } }
  direction: "forward"
```
### Random reordering with seed 9
`"HJ( ccc = ( COLLSCAN [test.base_coll_reorder_md_base] ), _ = ( HJ( _ = ( HJ( bbb = ( COLLSCAN [test.base_coll_reorder_md_b] ), _ = ( HJ( ddd = ( COLLSCAN [test.base_coll_reorder_md_b] ), _ = ( COLLSCAN [test.base_coll_reorder_md_base] ) ) ) ) ), aaa = ( COLLSCAN [test.base_coll_reorder_md_a] ) ) ) )"`
```
HASH_JOIN_EMBEDDING [base = aaa.base]
leftEmbeddingField: "ccc"
rightEmbeddingField: "none"
  |  |
  |  HASH_JOIN_EMBEDDING [a = a]
  |  leftEmbeddingField: "none"
  |  rightEmbeddingField: "aaa"
  |  |  |
  |  |  COLLSCAN [test.base_coll_reorder_md_a]
  |  |  filter: { "base" : { "$in" : [ 22, 33 ] } }
  |  |  direction: "forward"
  |  |
  |  HASH_JOIN_EMBEDDING [b = b]
  |  leftEmbeddingField: "bbb"
  |  rightEmbeddingField: "none"
  |  |  |
  |  |  HASH_JOIN_EMBEDDING [base = base]
  |  |  leftEmbeddingField: "ddd"
  |  |  rightEmbeddingField: "none"
  |  |  |  |
  |  |  |  COLLSCAN [test.base_coll_reorder_md_base]
  |  |  |  filter: { "b" : { "$eq" : 3 } }
  |  |  |  direction: "forward"
  |  |  |
  |  |  COLLSCAN [test.base_coll_reorder_md_b]
  |  |  filter: { "b" : { "$gt" : 0 } }
  |  |  direction: "forward"
  |  |
  |  COLLSCAN [test.base_coll_reorder_md_b]
  |  filter: { "$and" : [ { "b" : { "$eq" : 3 } }, { "base" : { "$gt" : 20 } } ] }
  |  direction: "forward"
  |
  COLLSCAN [test.base_coll_reorder_md_base]
  filter: { "$and" : [ { "b" : { "$lt" : 0 } }, { "base" : { "$in" : [ 22, 33 ] } } ] }
  direction: "forward"
```
### Random reordering with seed 10
`"HJ( ddd = ( COLLSCAN [test.base_coll_reorder_md_b] ), _ = ( HJ( _ = ( NLJ( _ = ( HJ( _ = ( COLLSCAN [test.base_coll_reorder_md_base] ), bbb = ( COLLSCAN [test.base_coll_reorder_md_b] ) ) ), aaa = ( COLLSCAN [test.base_coll_reorder_md_a] ) ) ), ccc = ( COLLSCAN [test.base_coll_reorder_md_base] ) ) ) )"`
```
HASH_JOIN_EMBEDDING [base = base]
leftEmbeddingField: "ddd"
rightEmbeddingField: "none"
  |  |
  |  HASH_JOIN_EMBEDDING [aaa.base = base]
  |  leftEmbeddingField: "none"
  |  rightEmbeddingField: "ccc"
  |  |  |
  |  |  COLLSCAN [test.base_coll_reorder_md_base]
  |  |  filter: { "$and" : [ { "b" : { "$lt" : 0 } }, { "base" : { "$in" : [ 22, 33 ] } } ] }
  |  |  direction: "forward"
  |  |
  |  NESTED_LOOP_JOIN_EMBEDDING [a = a]
  |  leftEmbeddingField: "none"
  |  rightEmbeddingField: "aaa"
  |  |  |
  |  |  COLLSCAN [test.base_coll_reorder_md_a]
  |  |  filter: { "base" : { "$in" : [ 22, 33 ] } }
  |  |  direction: "forward"
  |  |
  |  HASH_JOIN_EMBEDDING [b = b]
  |  leftEmbeddingField: "none"
  |  rightEmbeddingField: "bbb"
  |  |  |
  |  |  COLLSCAN [test.base_coll_reorder_md_b]
  |  |  filter: { "$and" : [ { "b" : { "$eq" : 3 } }, { "base" : { "$gt" : 20 } } ] }
  |  |  direction: "forward"
  |  |
  |  COLLSCAN [test.base_coll_reorder_md_base]
  |  filter: { "b" : { "$eq" : 3 } }
  |  direction: "forward"
  |
  COLLSCAN [test.base_coll_reorder_md_b]
  filter: { "b" : { "$gt" : 0 } }
  direction: "forward"
```
### Random reordering with seed 11
`"HJ( ccc = ( COLLSCAN [test.base_coll_reorder_md_base] ), _ = ( HJ( _ = ( HJ( ddd = ( COLLSCAN [test.base_coll_reorder_md_b] ), _ = ( HJ( aaa = ( COLLSCAN [test.base_coll_reorder_md_a] ), _ = ( COLLSCAN [test.base_coll_reorder_md_base] ) ) ) ) ), bbb = ( COLLSCAN [test.base_coll_reorder_md_b] ) ) ) )"`
```
HASH_JOIN_EMBEDDING [base = aaa.base]
leftEmbeddingField: "ccc"
rightEmbeddingField: "none"
  |  |
  |  HASH_JOIN_EMBEDDING [b = b]
  |  leftEmbeddingField: "none"
  |  rightEmbeddingField: "bbb"
  |  |  |
  |  |  COLLSCAN [test.base_coll_reorder_md_b]
  |  |  filter: { "$and" : [ { "b" : { "$eq" : 3 } }, { "base" : { "$gt" : 20 } } ] }
  |  |  direction: "forward"
  |  |
  |  HASH_JOIN_EMBEDDING [base = base]
  |  leftEmbeddingField: "ddd"
  |  rightEmbeddingField: "none"
  |  |  |
  |  |  HASH_JOIN_EMBEDDING [a = a]
  |  |  leftEmbeddingField: "aaa"
  |  |  rightEmbeddingField: "none"
  |  |  |  |
  |  |  |  COLLSCAN [test.base_coll_reorder_md_base]
  |  |  |  filter: { "b" : { "$eq" : 3 } }
  |  |  |  direction: "forward"
  |  |  |
  |  |  COLLSCAN [test.base_coll_reorder_md_a]
  |  |  filter: { "base" : { "$in" : [ 22, 33 ] } }
  |  |  direction: "forward"
  |  |
  |  COLLSCAN [test.base_coll_reorder_md_b]
  |  filter: { "b" : { "$gt" : 0 } }
  |  direction: "forward"
  |
  COLLSCAN [test.base_coll_reorder_md_base]
  filter: { "$and" : [ { "b" : { "$lt" : 0 } }, { "base" : { "$in" : [ 22, 33 ] } } ] }
  direction: "forward"
```

