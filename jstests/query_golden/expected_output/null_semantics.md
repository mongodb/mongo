## 1. Simple local-foreign field join
### No join opt
### Expected results
```json
{ "_id" : 0, "lf" : { "_id" : 0 } }
{ "_id" : 0, "lf" : { "_id" : 2, "key" : null } }
{ "_id" : 1, "key" : 1, "lf" : { "_id" : 1, "key" : 1 } }
{ "_id" : 2, "key" : null, "lf" : { "_id" : 0 } }
{ "_id" : 2, "key" : null, "lf" : { "_id" : 2, "key" : null } }
{ "_id" : 3, "key" : { }, "lf" : { "_id" : 3, "key" : { } } }
{ "_id" : 4, "key" : { "foo" : null }, "lf" : { "_id" : 4, "key" : { "foo" : null } } }
{ "_id" : 5, "key" : { "foo" : { } }, "lf" : { "_id" : 5, "key" : { "foo" : { } } } }
```
### internalJoinReorderMode = bottomUp, internalJoinPlanTreeShape = zigZag
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [key = key]
leftEmbeddingField: "none"
rightEmbeddingField: "lf"
  |  |
  |  COLLSCAN [test.null_semantics_md_other]
  |  direction: "forward"
  |
  COLLSCAN [test.null_semantics_md]
  direction: "forward"
```
### internalJoinReorderMode = random, internalRandomJoinReorderDefaultToHashJoin = true, internalRandomJoinOrderSeed = 42
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [key = key]
leftEmbeddingField: "none"
rightEmbeddingField: "lf"
  |  |
  |  COLLSCAN [test.null_semantics_md_other]
  |  direction: "forward"
  |
  COLLSCAN [test.null_semantics_md]
  direction: "forward"
```
### internalJoinReorderMode = random, internalRandomJoinReorderDefaultToHashJoin = false, internalRandomJoinOrderSeed = 64
usedJoinOptimization: true

```
NESTED_LOOP_JOIN_EMBEDDING [key = key]
leftEmbeddingField: "none"
rightEmbeddingField: "lf"
  |  |
  |  COLLSCAN [test.null_semantics_md_other]
  |  direction: "forward"
  |
  COLLSCAN [test.null_semantics_md]
  direction: "forward"
```
## 2. Simple local-foreign field join (nested field)
### No join opt
### Expected results
```json
{ "_id" : 0, "lf" : { "_id" : 0 } }
{ "_id" : 0, "lf" : { "_id" : 1, "key" : 1 } }
{ "_id" : 0, "lf" : { "_id" : 2, "key" : null } }
{ "_id" : 0, "lf" : { "_id" : 3, "key" : { } } }
{ "_id" : 0, "lf" : { "_id" : 4, "key" : { "foo" : null } } }
{ "_id" : 1, "key" : 1, "lf" : { "_id" : 0 } }
{ "_id" : 1, "key" : 1, "lf" : { "_id" : 1, "key" : 1 } }
{ "_id" : 1, "key" : 1, "lf" : { "_id" : 2, "key" : null } }
{ "_id" : 1, "key" : 1, "lf" : { "_id" : 3, "key" : { } } }
{ "_id" : 1, "key" : 1, "lf" : { "_id" : 4, "key" : { "foo" : null } } }
{ "_id" : 2, "key" : null, "lf" : { "_id" : 0 } }
{ "_id" : 2, "key" : null, "lf" : { "_id" : 1, "key" : 1 } }
{ "_id" : 2, "key" : null, "lf" : { "_id" : 2, "key" : null } }
{ "_id" : 2, "key" : null, "lf" : { "_id" : 3, "key" : { } } }
{ "_id" : 2, "key" : null, "lf" : { "_id" : 4, "key" : { "foo" : null } } }
{ "_id" : 3, "key" : { }, "lf" : { "_id" : 0 } }
{ "_id" : 3, "key" : { }, "lf" : { "_id" : 1, "key" : 1 } }
{ "_id" : 3, "key" : { }, "lf" : { "_id" : 2, "key" : null } }
{ "_id" : 3, "key" : { }, "lf" : { "_id" : 3, "key" : { } } }
{ "_id" : 3, "key" : { }, "lf" : { "_id" : 4, "key" : { "foo" : null } } }
{ "_id" : 4, "key" : { "foo" : null }, "lf" : { "_id" : 0 } }
{ "_id" : 4, "key" : { "foo" : null }, "lf" : { "_id" : 1, "key" : 1 } }
{ "_id" : 4, "key" : { "foo" : null }, "lf" : { "_id" : 2, "key" : null } }
{ "_id" : 4, "key" : { "foo" : null }, "lf" : { "_id" : 3, "key" : { } } }
{ "_id" : 4, "key" : { "foo" : null }, "lf" : { "_id" : 4, "key" : { "foo" : null } } }
{ "_id" : 5, "key" : { "foo" : { } }, "lf" : { "_id" : 5, "key" : { "foo" : { } } } }
```
### internalJoinReorderMode = bottomUp, internalJoinPlanTreeShape = zigZag
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [key.foo = key.foo]
leftEmbeddingField: "none"
rightEmbeddingField: "lf"
  |  |
  |  COLLSCAN [test.null_semantics_md_other]
  |  direction: "forward"
  |
  COLLSCAN [test.null_semantics_md]
  direction: "forward"
```
### internalJoinReorderMode = random, internalRandomJoinReorderDefaultToHashJoin = true, internalRandomJoinOrderSeed = 42
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [key.foo = key.foo]
leftEmbeddingField: "none"
rightEmbeddingField: "lf"
  |  |
  |  COLLSCAN [test.null_semantics_md_other]
  |  direction: "forward"
  |
  COLLSCAN [test.null_semantics_md]
  direction: "forward"
```
### internalJoinReorderMode = random, internalRandomJoinReorderDefaultToHashJoin = false, internalRandomJoinOrderSeed = 64
usedJoinOptimization: true

```
NESTED_LOOP_JOIN_EMBEDDING [key.foo = key.foo]
leftEmbeddingField: "none"
rightEmbeddingField: "lf"
  |  |
  |  COLLSCAN [test.null_semantics_md_other]
  |  direction: "forward"
  |
  COLLSCAN [test.null_semantics_md]
  direction: "forward"
```
## 3. Correlated sub-pipeline
### No join opt
### Expected results
```json
{ "_id" : 0, "cor" : { "_id" : 0 } }
{ "_id" : 1, "cor" : { "_id" : 1, "key" : 1 }, "key" : 1 }
{ "_id" : 2, "cor" : { "_id" : 2, "key" : null }, "key" : null }
{ "_id" : 3, "cor" : { "_id" : 3, "key" : { } }, "key" : { } }
{ "_id" : 4, "cor" : { "_id" : 4, "key" : { "foo" : null } }, "key" : { "foo" : null } }
{ "_id" : 5, "cor" : { "_id" : 5, "key" : { "foo" : { } } }, "key" : { "foo" : { } } }
```
### internalJoinReorderMode = bottomUp, internalJoinPlanTreeShape = zigZag
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [key $= key]
leftEmbeddingField: "none"
rightEmbeddingField: "cor"
  |  |
  |  COLLSCAN [test.null_semantics_md_other]
  |  direction: "forward"
  |
  COLLSCAN [test.null_semantics_md]
  direction: "forward"
```
### internalJoinReorderMode = random, internalRandomJoinReorderDefaultToHashJoin = true, internalRandomJoinOrderSeed = 42
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [key $= key]
leftEmbeddingField: "none"
rightEmbeddingField: "cor"
  |  |
  |  COLLSCAN [test.null_semantics_md_other]
  |  direction: "forward"
  |
  COLLSCAN [test.null_semantics_md]
  direction: "forward"
```
### internalJoinReorderMode = random, internalRandomJoinReorderDefaultToHashJoin = false, internalRandomJoinOrderSeed = 64
usedJoinOptimization: true

```
NESTED_LOOP_JOIN_EMBEDDING [key $= key]
leftEmbeddingField: "none"
rightEmbeddingField: "cor"
  |  |
  |  COLLSCAN [test.null_semantics_md_other]
  |  direction: "forward"
  |
  COLLSCAN [test.null_semantics_md]
  direction: "forward"
```
## 4. Correlated sub-pipeline (nested field)
### No join opt
### Expected results
```json
{ "_id" : 0, "cor" : { "_id" : 0 } }
{ "_id" : 0, "cor" : { "_id" : 1, "key" : 1 } }
{ "_id" : 0, "cor" : { "_id" : 2, "key" : null } }
{ "_id" : 0, "cor" : { "_id" : 3, "key" : { } } }
{ "_id" : 1, "cor" : { "_id" : 0 }, "key" : 1 }
{ "_id" : 1, "cor" : { "_id" : 1, "key" : 1 }, "key" : 1 }
{ "_id" : 1, "cor" : { "_id" : 2, "key" : null }, "key" : 1 }
{ "_id" : 1, "cor" : { "_id" : 3, "key" : { } }, "key" : 1 }
{ "_id" : 2, "cor" : { "_id" : 0 }, "key" : null }
{ "_id" : 2, "cor" : { "_id" : 1, "key" : 1 }, "key" : null }
{ "_id" : 2, "cor" : { "_id" : 2, "key" : null }, "key" : null }
{ "_id" : 2, "cor" : { "_id" : 3, "key" : { } }, "key" : null }
{ "_id" : 3, "cor" : { "_id" : 0 }, "key" : { } }
{ "_id" : 3, "cor" : { "_id" : 1, "key" : 1 }, "key" : { } }
{ "_id" : 3, "cor" : { "_id" : 2, "key" : null }, "key" : { } }
{ "_id" : 3, "cor" : { "_id" : 3, "key" : { } }, "key" : { } }
{ "_id" : 4, "cor" : { "_id" : 4, "key" : { "foo" : null } }, "key" : { "foo" : null } }
{ "_id" : 5, "cor" : { "_id" : 5, "key" : { "foo" : { } } }, "key" : { "foo" : { } } }
```
### internalJoinReorderMode = bottomUp, internalJoinPlanTreeShape = zigZag
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [key.foo $= key.foo]
leftEmbeddingField: "none"
rightEmbeddingField: "cor"
  |  |
  |  COLLSCAN [test.null_semantics_md_other]
  |  direction: "forward"
  |
  COLLSCAN [test.null_semantics_md]
  direction: "forward"
```
### internalJoinReorderMode = random, internalRandomJoinReorderDefaultToHashJoin = true, internalRandomJoinOrderSeed = 42
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [key.foo $= key.foo]
leftEmbeddingField: "none"
rightEmbeddingField: "cor"
  |  |
  |  COLLSCAN [test.null_semantics_md_other]
  |  direction: "forward"
  |
  COLLSCAN [test.null_semantics_md]
  direction: "forward"
```
### internalJoinReorderMode = random, internalRandomJoinReorderDefaultToHashJoin = false, internalRandomJoinOrderSeed = 64
usedJoinOptimization: true

```
NESTED_LOOP_JOIN_EMBEDDING [key.foo $= key.foo]
leftEmbeddingField: "none"
rightEmbeddingField: "cor"
  |  |
  |  COLLSCAN [test.null_semantics_md_other]
  |  direction: "forward"
  |
  COLLSCAN [test.null_semantics_md]
  direction: "forward"
```
## 5. Implicit cycle (local-foreign)
### No join opt
### Expected results
```json
{ "_id" : 0, "lf" : { "_id" : 0 }, "lf2" : { "_id" : 0 } }
{ "_id" : 0, "lf" : { "_id" : 0 }, "lf2" : { "_id" : 2, "key" : null } }
{ "_id" : 0, "lf" : { "_id" : 2, "key" : null }, "lf2" : { "_id" : 0 } }
{ "_id" : 0, "lf" : { "_id" : 2, "key" : null }, "lf2" : { "_id" : 2, "key" : null } }
{ "_id" : 1, "key" : 1, "lf" : { "_id" : 1, "key" : 1 }, "lf2" : { "_id" : 1, "key" : 1 } }
{ "_id" : 2, "key" : null, "lf" : { "_id" : 0 }, "lf2" : { "_id" : 0 } }
{ "_id" : 2, "key" : null, "lf" : { "_id" : 0 }, "lf2" : { "_id" : 2, "key" : null } }
{ "_id" : 2, "key" : null, "lf" : { "_id" : 2, "key" : null }, "lf2" : { "_id" : 0 } }
{ "_id" : 2, "key" : null, "lf" : { "_id" : 2, "key" : null }, "lf2" : { "_id" : 2, "key" : null } }
{ "_id" : 3, "key" : { }, "lf" : { "_id" : 3, "key" : { } }, "lf2" : { "_id" : 3, "key" : { } } }
{ "_id" : 4, "key" : { "foo" : null }, "lf" : { "_id" : 4, "key" : { "foo" : null } }, "lf2" : { "_id" : 4, "key" : { "foo" : null } } }
{ "_id" : 5, "key" : { "foo" : { } }, "lf" : { "_id" : 5, "key" : { "foo" : { } } }, "lf2" : { "_id" : 5, "key" : { "foo" : { } } } }
```
### internalJoinReorderMode = bottomUp, internalJoinPlanTreeShape = zigZag
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [lf.key = key,key = key]
leftEmbeddingField: "none"
rightEmbeddingField: "lf2"
  |  |
  |  COLLSCAN [test.null_semantics_md_third]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [key = key]
  leftEmbeddingField: "none"
  rightEmbeddingField: "lf"
  |  |
  |  COLLSCAN [test.null_semantics_md_other]
  |  direction: "forward"
  |
  COLLSCAN [test.null_semantics_md]
  direction: "forward"
```
### internalJoinReorderMode = random, internalRandomJoinReorderDefaultToHashJoin = true, internalRandomJoinOrderSeed = 42
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [lf.key = key,key = key]
leftEmbeddingField: "none"
rightEmbeddingField: "lf2"
  |  |
  |  COLLSCAN [test.null_semantics_md_third]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [key = key]
  leftEmbeddingField: "none"
  rightEmbeddingField: "lf"
  |  |
  |  COLLSCAN [test.null_semantics_md_other]
  |  direction: "forward"
  |
  COLLSCAN [test.null_semantics_md]
  direction: "forward"
```
### internalJoinReorderMode = random, internalRandomJoinReorderDefaultToHashJoin = false, internalRandomJoinOrderSeed = 64
usedJoinOptimization: true

```
NESTED_LOOP_JOIN_EMBEDDING [lf.key = key,lf2.key = key]
leftEmbeddingField: "none"
rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.null_semantics_md]
  |  direction: "forward"
  |
  NESTED_LOOP_JOIN_EMBEDDING [key = key]
  leftEmbeddingField: "lf2"
  rightEmbeddingField: "lf"
  |  |
  |  COLLSCAN [test.null_semantics_md_other]
  |  direction: "forward"
  |
  COLLSCAN [test.null_semantics_md_third]
  direction: "forward"
```
## 6. Implicit cycle (mixed)
### No join opt
### Expected results
```json
{ "_id" : 0, "cor" : { "_id" : 0 }, "lf" : { "_id" : 0 } }
{ "_id" : 0, "cor" : { "_id" : 1, "key" : 1 }, "lf" : { "_id" : 0 } }
{ "_id" : 0, "cor" : { "_id" : 2, "key" : null }, "lf" : { "_id" : 0 } }
{ "_id" : 0, "cor" : { "_id" : 3, "key" : { } }, "lf" : { "_id" : 0 } }
{ "_id" : 0, "cor" : { "_id" : 4, "key" : { "foo" : null } }, "lf" : { "_id" : 2, "key" : null } }
{ "_id" : 2, "cor" : { "_id" : 0 }, "key" : null, "lf" : { "_id" : 0 } }
{ "_id" : 2, "cor" : { "_id" : 1, "key" : 1 }, "key" : null, "lf" : { "_id" : 0 } }
{ "_id" : 2, "cor" : { "_id" : 2, "key" : null }, "key" : null, "lf" : { "_id" : 0 } }
{ "_id" : 2, "cor" : { "_id" : 3, "key" : { } }, "key" : null, "lf" : { "_id" : 0 } }
{ "_id" : 2, "cor" : { "_id" : 4, "key" : { "foo" : null } }, "key" : null, "lf" : { "_id" : 2, "key" : null } }
{ "_id" : 3, "cor" : { "_id" : 5, "key" : { "foo" : { } } }, "key" : { }, "lf" : { "_id" : 3, "key" : { } } }
```
### internalJoinReorderMode = bottomUp, internalJoinPlanTreeShape = zigZag
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [lf.key $= key.foo,key = key.foo]
leftEmbeddingField: "none"
rightEmbeddingField: "cor"
  |  |
  |  COLLSCAN [test.null_semantics_md_third]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [key = key]
  leftEmbeddingField: "none"
  rightEmbeddingField: "lf"
  |  |
  |  COLLSCAN [test.null_semantics_md_other]
  |  direction: "forward"
  |
  COLLSCAN [test.null_semantics_md]
  direction: "forward"
```
### internalJoinReorderMode = random, internalRandomJoinReorderDefaultToHashJoin = true, internalRandomJoinOrderSeed = 42
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [lf.key $= key.foo,key = key.foo]
leftEmbeddingField: "none"
rightEmbeddingField: "cor"
  |  |
  |  COLLSCAN [test.null_semantics_md_third]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [key = key]
  leftEmbeddingField: "none"
  rightEmbeddingField: "lf"
  |  |
  |  COLLSCAN [test.null_semantics_md_other]
  |  direction: "forward"
  |
  COLLSCAN [test.null_semantics_md]
  direction: "forward"
```
### internalJoinReorderMode = random, internalRandomJoinReorderDefaultToHashJoin = false, internalRandomJoinOrderSeed = 64
usedJoinOptimization: true

```
NESTED_LOOP_JOIN_EMBEDDING [lf.key = key,cor.key.foo = key]
leftEmbeddingField: "none"
rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.null_semantics_md]
  |  direction: "forward"
  |
  NESTED_LOOP_JOIN_EMBEDDING [key.foo $= key]
  leftEmbeddingField: "cor"
  rightEmbeddingField: "lf"
  |  |
  |  COLLSCAN [test.null_semantics_md_other]
  |  direction: "forward"
  |
  COLLSCAN [test.null_semantics_md_third]
  direction: "forward"
```
## 7. Implicit cycle (correlated)
### No join opt
### Expected results
```json
{ "_id" : 0, "cor" : { "_id" : 0 }, "cor2" : { "_id" : 0 } }
{ "_id" : 0, "cor" : { "_id" : 0 }, "cor2" : { "_id" : 1, "key" : 1 } }
{ "_id" : 0, "cor" : { "_id" : 0 }, "cor2" : { "_id" : 2, "key" : null } }
{ "_id" : 0, "cor" : { "_id" : 0 }, "cor2" : { "_id" : 3, "key" : { } } }
{ "_id" : 1, "cor" : { "_id" : 1, "key" : 1 }, "cor2" : { "_id" : 0 }, "key" : 1 }
{ "_id" : 1, "cor" : { "_id" : 1, "key" : 1 }, "cor2" : { "_id" : 1, "key" : 1 }, "key" : 1 }
{ "_id" : 1, "cor" : { "_id" : 1, "key" : 1 }, "cor2" : { "_id" : 2, "key" : null }, "key" : 1 }
{ "_id" : 1, "cor" : { "_id" : 1, "key" : 1 }, "cor2" : { "_id" : 3, "key" : { } }, "key" : 1 }
{ "_id" : 2, "cor" : { "_id" : 2, "key" : null }, "cor2" : { "_id" : 0 }, "key" : null }
{ "_id" : 2, "cor" : { "_id" : 2, "key" : null }, "cor2" : { "_id" : 1, "key" : 1 }, "key" : null }
{ "_id" : 2, "cor" : { "_id" : 2, "key" : null }, "cor2" : { "_id" : 2, "key" : null }, "key" : null }
{ "_id" : 2, "cor" : { "_id" : 2, "key" : null }, "cor2" : { "_id" : 3, "key" : { } }, "key" : null }
{ "_id" : 3, "cor" : { "_id" : 3, "key" : { } }, "cor2" : { "_id" : 0 }, "key" : { } }
{ "_id" : 3, "cor" : { "_id" : 3, "key" : { } }, "cor2" : { "_id" : 1, "key" : 1 }, "key" : { } }
{ "_id" : 3, "cor" : { "_id" : 3, "key" : { } }, "cor2" : { "_id" : 2, "key" : null }, "key" : { } }
{ "_id" : 3, "cor" : { "_id" : 3, "key" : { } }, "cor2" : { "_id" : 3, "key" : { } }, "key" : { } }
{ "_id" : 4, "cor" : { "_id" : 4, "key" : { "foo" : null } }, "cor2" : { "_id" : 0 }, "key" : { "foo" : null } }
{ "_id" : 4, "cor" : { "_id" : 4, "key" : { "foo" : null } }, "cor2" : { "_id" : 1, "key" : 1 }, "key" : { "foo" : null } }
{ "_id" : 4, "cor" : { "_id" : 4, "key" : { "foo" : null } }, "cor2" : { "_id" : 2, "key" : null }, "key" : { "foo" : null } }
{ "_id" : 4, "cor" : { "_id" : 4, "key" : { "foo" : null } }, "cor2" : { "_id" : 3, "key" : { } }, "key" : { "foo" : null } }
{ "_id" : 5, "cor" : { "_id" : 5, "key" : { "foo" : { } } }, "cor2" : { "_id" : 0 }, "key" : { "foo" : { } } }
{ "_id" : 5, "cor" : { "_id" : 5, "key" : { "foo" : { } } }, "cor2" : { "_id" : 1, "key" : 1 }, "key" : { "foo" : { } } }
{ "_id" : 5, "cor" : { "_id" : 5, "key" : { "foo" : { } } }, "cor2" : { "_id" : 2, "key" : null }, "key" : { "foo" : { } } }
{ "_id" : 5, "cor" : { "_id" : 5, "key" : { "foo" : { } } }, "cor2" : { "_id" : 3, "key" : { } }, "key" : { "foo" : { } } }
```
### internalJoinReorderMode = bottomUp, internalJoinPlanTreeShape = zigZag
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [cor.foo $= key.foo]
leftEmbeddingField: "none"
rightEmbeddingField: "cor2"
  |  |
  |  COLLSCAN [test.null_semantics_md_other]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [key $= key]
  leftEmbeddingField: "none"
  rightEmbeddingField: "cor"
  |  |
  |  COLLSCAN [test.null_semantics_md_third]
  |  direction: "forward"
  |
  COLLSCAN [test.null_semantics_md]
  direction: "forward"
```
### internalJoinReorderMode = random, internalRandomJoinReorderDefaultToHashJoin = true, internalRandomJoinOrderSeed = 42
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [cor.foo $= key.foo]
leftEmbeddingField: "none"
rightEmbeddingField: "cor2"
  |  |
  |  COLLSCAN [test.null_semantics_md_other]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [key $= key]
  leftEmbeddingField: "none"
  rightEmbeddingField: "cor"
  |  |
  |  COLLSCAN [test.null_semantics_md_third]
  |  direction: "forward"
  |
  COLLSCAN [test.null_semantics_md]
  direction: "forward"
```
### internalJoinReorderMode = random, internalRandomJoinReorderDefaultToHashJoin = false, internalRandomJoinOrderSeed = 64
usedJoinOptimization: true

```
NESTED_LOOP_JOIN_EMBEDDING [cor.key $= key]
leftEmbeddingField: "none"
rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.null_semantics_md]
  |  direction: "forward"
  |
  NESTED_LOOP_JOIN_EMBEDDING [key.foo $= foo]
  leftEmbeddingField: "cor2"
  rightEmbeddingField: "cor"
  |  |
  |  COLLSCAN [test.null_semantics_md_third]
  |  direction: "forward"
  |
  COLLSCAN [test.null_semantics_md_other]
  direction: "forward"
```
## 8. Implicit cycle (mixed) + indexes
### No join opt
### Expected results
```json
{ "_id" : 0, "cor" : { "_id" : 0 }, "lf" : { "_id" : 0 } }
{ "_id" : 0, "cor" : { "_id" : 1, "key" : 1 }, "lf" : { "_id" : 0 } }
{ "_id" : 0, "cor" : { "_id" : 2, "key" : null }, "lf" : { "_id" : 0 } }
{ "_id" : 0, "cor" : { "_id" : 3, "key" : { } }, "lf" : { "_id" : 0 } }
{ "_id" : 0, "cor" : { "_id" : 4, "key" : { "foo" : null } }, "lf" : { "_id" : 2, "key" : null } }
{ "_id" : 2, "cor" : { "_id" : 0 }, "key" : null, "lf" : { "_id" : 0 } }
{ "_id" : 2, "cor" : { "_id" : 1, "key" : 1 }, "key" : null, "lf" : { "_id" : 0 } }
{ "_id" : 2, "cor" : { "_id" : 2, "key" : null }, "key" : null, "lf" : { "_id" : 0 } }
{ "_id" : 2, "cor" : { "_id" : 3, "key" : { } }, "key" : null, "lf" : { "_id" : 0 } }
{ "_id" : 2, "cor" : { "_id" : 4, "key" : { "foo" : null } }, "key" : null, "lf" : { "_id" : 2, "key" : null } }
{ "_id" : 3, "cor" : { "_id" : 5, "key" : { "foo" : { } } }, "key" : { }, "lf" : { "_id" : 3, "key" : { } } }
```
### internalJoinReorderMode = bottomUp, internalJoinPlanTreeShape = zigZag
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [lf.key $= key.foo,key = key.foo]
leftEmbeddingField: "none"
rightEmbeddingField: "cor"
  |  |
  |  COLLSCAN [test.null_semantics_md_third]
  |  direction: "forward"
  |
  INDEXED_NESTED_LOOP_JOIN_EMBEDDING [key = key]
  leftEmbeddingField: "none"
  rightEmbeddingField: "lf"
  |  |
  |  FETCH [test.null_semantics_md_other]
  |  
  |  |
  |  INDEX_PROBE_NODE [test.null_semantics_md_other]
  |  keyPattern: { "key" : 1 }
  |  indexName: "key_1"
  |  isMultiKey: false
  |  isUnique: false
  |  isSparse: false
  |  isPartial: false
  |
  COLLSCAN [test.null_semantics_md]
  direction: "forward"
```
### internalJoinReorderMode = random, internalRandomJoinReorderDefaultToHashJoin = true, internalRandomJoinOrderSeed = 42
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [lf.key $= key.foo,key = key.foo]
leftEmbeddingField: "none"
rightEmbeddingField: "cor"
  |  |
  |  COLLSCAN [test.null_semantics_md_third]
  |  direction: "forward"
  |
  INDEXED_NESTED_LOOP_JOIN_EMBEDDING [key = key]
  leftEmbeddingField: "none"
  rightEmbeddingField: "lf"
  |  |
  |  FETCH [test.null_semantics_md_other]
  |  
  |  |
  |  INDEX_PROBE_NODE [test.null_semantics_md_other]
  |  keyPattern: { "key" : 1 }
  |  indexName: "key_1"
  |  isMultiKey: false
  |  isUnique: false
  |  isSparse: false
  |  isPartial: false
  |
  COLLSCAN [test.null_semantics_md]
  direction: "forward"
```
### internalJoinReorderMode = random, internalRandomJoinReorderDefaultToHashJoin = false, internalRandomJoinOrderSeed = 64
usedJoinOptimization: true

```
NESTED_LOOP_JOIN_EMBEDDING [lf.key = key,cor.key.foo = key]
leftEmbeddingField: "none"
rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.null_semantics_md]
  |  direction: "forward"
  |
  INDEXED_NESTED_LOOP_JOIN_EMBEDDING [key.foo $= key]
  leftEmbeddingField: "cor"
  rightEmbeddingField: "lf"
  |  |
  |  FETCH [test.null_semantics_md_other]
  |  
  |  |
  |  INDEX_PROBE_NODE [test.null_semantics_md_other]
  |  keyPattern: { "key" : 1 }
  |  indexName: "key_1"
  |  isMultiKey: false
  |  isUnique: false
  |  isSparse: false
  |  isPartial: false
  |
  COLLSCAN [test.null_semantics_md_third]
  direction: "forward"
```
## 9. Large implicit cycle (5 nodes)
### No join opt
### Expected results
```json
{ "_id" : 0, "c1" : { "_id" : 0 }, "c2" : { "_id" : 0 }, "c3" : { "_id" : 0 }, "c4" : { "_id" : 0 } }
{ "_id" : 1, "c1" : { "_id" : 1, "key" : 1 }, "c2" : { "_id" : 1, "key" : 1 }, "c3" : { "_id" : 1, "key" : 1 }, "c4" : { "_id" : 1, "key" : 1 }, "key" : 1 }
{ "_id" : 2, "c1" : { "_id" : 2, "key" : null }, "c2" : { "_id" : 2, "key" : null }, "c3" : { "_id" : 2, "key" : null }, "c4" : { "_id" : 2, "key" : null }, "key" : null }
{ "_id" : 3, "c1" : { "_id" : 3, "key" : { } }, "c2" : { "_id" : 3, "key" : { } }, "c3" : { "_id" : 3, "key" : { } }, "c4" : { "_id" : 3, "key" : { } }, "key" : { } }
{ "_id" : 4, "c1" : { "_id" : 4, "key" : { "foo" : null } }, "c2" : { "_id" : 4, "key" : { "foo" : null } }, "c3" : { "_id" : 4, "key" : { "foo" : null } }, "c4" : { "_id" : 4, "key" : { "foo" : null } }, "key" : { "foo" : null } }
{ "_id" : 5, "c1" : { "_id" : 5, "key" : { "foo" : { } } }, "c2" : { "_id" : 5, "key" : { "foo" : { } } }, "c3" : { "_id" : 5, "key" : { "foo" : { } } }, "c4" : { "_id" : 5, "key" : { "foo" : { } } }, "key" : { "foo" : { } } }
```
### internalJoinReorderMode = bottomUp, internalJoinPlanTreeShape = zigZag
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [c3.key $= key,key = key,c1.key = key,c2.key = key]
leftEmbeddingField: "none"
rightEmbeddingField: "c4"
  |  |
  |  COLLSCAN [test.null_semantics_md_fifth]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [c2.key $= key,key = key,c1.key = key]
  leftEmbeddingField: "none"
  rightEmbeddingField: "c3"
  |  |
  |  COLLSCAN [test.null_semantics_md_fourth]
  |  direction: "forward"
  |
  INDEXED_NESTED_LOOP_JOIN_EMBEDDING [c1.key $= key,key = key]
  leftEmbeddingField: "none"
  rightEmbeddingField: "c2"
  |  |
  |  FETCH [test.null_semantics_md_third]
  |  
  |  |
  |  INDEX_PROBE_NODE [test.null_semantics_md_third]
  |  keyPattern: { "key" : 1 }
  |  indexName: "key_1"
  |  isMultiKey: false
  |  isUnique: false
  |  isSparse: false
  |  isPartial: false
  |
  INDEXED_NESTED_LOOP_JOIN_EMBEDDING [key $= key]
  leftEmbeddingField: "none"
  rightEmbeddingField: "c1"
  |  |
  |  FETCH [test.null_semantics_md_other]
  |  
  |  |
  |  INDEX_PROBE_NODE [test.null_semantics_md_other]
  |  keyPattern: { "key" : 1 }
  |  indexName: "key_1"
  |  isMultiKey: false
  |  isUnique: false
  |  isSparse: false
  |  isPartial: false
  |
  COLLSCAN [test.null_semantics_md]
  direction: "forward"
```
### internalJoinReorderMode = random, internalRandomJoinReorderDefaultToHashJoin = true, internalRandomJoinOrderSeed = 42
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [c2.key $= key,c4.key $= key,key = key,c1.key = key]
leftEmbeddingField: "none"
rightEmbeddingField: "c3"
  |  |
  |  COLLSCAN [test.null_semantics_md_fourth]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [key = key,c1.key = key,c2.key = key]
  leftEmbeddingField: "none"
  rightEmbeddingField: "c4"
  |  |
  |  COLLSCAN [test.null_semantics_md_fifth]
  |  direction: "forward"
  |
  INDEXED_NESTED_LOOP_JOIN_EMBEDDING [key $= key,c2.key $= key]
  leftEmbeddingField: "none"
  rightEmbeddingField: "c1"
  |  |
  |  FETCH [test.null_semantics_md_other]
  |  
  |  |
  |  INDEX_PROBE_NODE [test.null_semantics_md_other]
  |  keyPattern: { "key" : 1 }
  |  indexName: "key_1"
  |  isMultiKey: false
  |  isUnique: false
  |  isSparse: false
  |  isPartial: false
  |
  HASH_JOIN_EMBEDDING [key = key]
  leftEmbeddingField: "c2"
  rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.null_semantics_md]
  |  direction: "forward"
  |
  COLLSCAN [test.null_semantics_md_third]
  direction: "forward"
```
### internalJoinReorderMode = random, internalRandomJoinReorderDefaultToHashJoin = false, internalRandomJoinOrderSeed = 64
usedJoinOptimization: true

```
INDEXED_NESTED_LOOP_JOIN_EMBEDDING [key $= key,c2.key $= key,c3.key = key,c4.key = key]
leftEmbeddingField: "none"
rightEmbeddingField: "c1"
  |  |
  |  FETCH [test.null_semantics_md_other]
  |  
  |  |
  |  INDEX_PROBE_NODE [test.null_semantics_md_other]
  |  keyPattern: { "key" : 1 }
  |  indexName: "key_1"
  |  isMultiKey: false
  |  isUnique: false
  |  isSparse: false
  |  isPartial: false
  |
  NESTED_LOOP_JOIN_EMBEDDING [c2.key = key,c3.key = key,c4.key = key]
  leftEmbeddingField: "none"
  rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.null_semantics_md]
  |  direction: "forward"
  |
  INDEXED_NESTED_LOOP_JOIN_EMBEDDING [c3.key $= key,c4.key = key]
  leftEmbeddingField: "none"
  rightEmbeddingField: "c2"
  |  |
  |  FETCH [test.null_semantics_md_third]
  |  
  |  |
  |  INDEX_PROBE_NODE [test.null_semantics_md_third]
  |  keyPattern: { "key" : 1 }
  |  indexName: "key_1"
  |  isMultiKey: false
  |  isUnique: false
  |  isSparse: false
  |  isPartial: false
  |
  NESTED_LOOP_JOIN_EMBEDDING [key $= key]
  leftEmbeddingField: "c3"
  rightEmbeddingField: "c4"
  |  |
  |  COLLSCAN [test.null_semantics_md_fifth]
  |  direction: "forward"
  |
  COLLSCAN [test.null_semantics_md_fourth]
  direction: "forward"
```
