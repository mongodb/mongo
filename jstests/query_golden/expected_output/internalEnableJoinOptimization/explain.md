## 1. Test getting rejected plan out of explain
### Cheapest plan (no ALL plans enum)
usedJoinOptimization: true

### Winning plan
```
NESTED_LOOP_JOIN_EMBEDDING [a2.a = a,a = a,a1.a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "a3"
  |  |
  |  COLLSCAN [test.explain_md]
  |  direction: "forward"
  |
  NESTED_LOOP_JOIN_EMBEDDING [a1.a = a,a = a]
  leftEmbeddingField: "none"
  rightEmbeddingField: "a2"
  |  |
  |  COLLSCAN [test.explain_md]
  |  direction: "forward"
  |
  NESTED_LOOP_JOIN_EMBEDDING [a = a]
  leftEmbeddingField: "none"
  rightEmbeddingField: "a1"
  |  |
  |  COLLSCAN [test.explain_md]
  |  direction: "forward"
  |
  COLLSCAN [test.explain_md]
  direction: "forward"
```
### Rejected plans (total: 1)
```
HASH_JOIN_EMBEDDING [a2.a = a,a = a,a1.a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "a3"
  |  |
  |  COLLSCAN [test.explain_md]
  |  direction: "forward"
  |
  NESTED_LOOP_JOIN_EMBEDDING [a1.a = a,a = a]
  leftEmbeddingField: "none"
  rightEmbeddingField: "a2"
  |  |
  |  COLLSCAN [test.explain_md]
  |  direction: "forward"
  |
  NESTED_LOOP_JOIN_EMBEDDING [a = a]
  leftEmbeddingField: "none"
  rightEmbeddingField: "a1"
  |  |
  |  COLLSCAN [test.explain_md]
  |  direction: "forward"
  |
  COLLSCAN [test.explain_md]
  direction: "forward"
```

### ALL plans, subset level 0 only
usedJoinOptimization: true

### Rejected plans (total: 1)
```
HASH_JOIN_EMBEDDING [a2.a = a,a = a,a1.a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "a3"
  |  |
  |  COLLSCAN [test.explain_md]
  |  direction: "forward"
  |
  NESTED_LOOP_JOIN_EMBEDDING [a1.a = a,a = a]
  leftEmbeddingField: "none"
  rightEmbeddingField: "a2"
  |  |
  |  COLLSCAN [test.explain_md]
  |  direction: "forward"
  |
  NESTED_LOOP_JOIN_EMBEDDING [a = a]
  leftEmbeddingField: "none"
  rightEmbeddingField: "a1"
  |  |
  |  COLLSCAN [test.explain_md]
  |  direction: "forward"
  |
  COLLSCAN [test.explain_md]
  direction: "forward"
```

### ALL plans, subset level 1 only 
usedJoinOptimization: true

### Rejected plans (total: 1)
```
HASH_JOIN_EMBEDDING [a2.a = a,a = a,a1.a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "a3"
  |  |
  |  COLLSCAN [test.explain_md]
  |  direction: "forward"
  |
  NESTED_LOOP_JOIN_EMBEDDING [a1.a = a,a = a]
  leftEmbeddingField: "none"
  rightEmbeddingField: "a2"
  |  |
  |  COLLSCAN [test.explain_md]
  |  direction: "forward"
  |
  NESTED_LOOP_JOIN_EMBEDDING [a = a]
  leftEmbeddingField: "none"
  rightEmbeddingField: "a1"
  |  |
  |  COLLSCAN [test.explain_md]
  |  direction: "forward"
  |
  COLLSCAN [test.explain_md]
  direction: "forward"
```

### ALL plans, subset level 2 only 
usedJoinOptimization: true

### Rejected plans (total: 1)
```
HASH_JOIN_EMBEDDING [a2.a = a,a = a,a1.a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "a3"
  |  |
  |  COLLSCAN [test.explain_md]
  |  direction: "forward"
  |
  NESTED_LOOP_JOIN_EMBEDDING [a1.a = a,a = a]
  leftEmbeddingField: "none"
  rightEmbeddingField: "a2"
  |  |
  |  COLLSCAN [test.explain_md]
  |  direction: "forward"
  |
  NESTED_LOOP_JOIN_EMBEDDING [a = a]
  leftEmbeddingField: "none"
  rightEmbeddingField: "a1"
  |  |
  |  COLLSCAN [test.explain_md]
  |  direction: "forward"
  |
  COLLSCAN [test.explain_md]
  direction: "forward"
```

### ALL plans, subset level 3 only 
usedJoinOptimization: true

### Rejected plans (total: 23)
Too many plans, printing only top 3 by cost.
```
NESTED_LOOP_JOIN_EMBEDDING [a1.a = a,a3.a = a,a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "a2"
  |  |
  |  COLLSCAN [test.explain_md]
  |  direction: "forward"
  |
  NESTED_LOOP_JOIN_EMBEDDING [a = a,a1.a = a]
  leftEmbeddingField: "none"
  rightEmbeddingField: "a3"
  |  |
  |  COLLSCAN [test.explain_md]
  |  direction: "forward"
  |
  NESTED_LOOP_JOIN_EMBEDDING [a = a]
  leftEmbeddingField: "none"
  rightEmbeddingField: "a1"
  |  |
  |  COLLSCAN [test.explain_md]
  |  direction: "forward"
  |
  COLLSCAN [test.explain_md]
  direction: "forward"
```
```
NESTED_LOOP_JOIN_EMBEDDING [a = a,a2.a = a,a3.a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "a1"
  |  |
  |  COLLSCAN [test.explain_md]
  |  direction: "forward"
  |
  NESTED_LOOP_JOIN_EMBEDDING [a2.a = a,a = a]
  leftEmbeddingField: "none"
  rightEmbeddingField: "a3"
  |  |
  |  COLLSCAN [test.explain_md]
  |  direction: "forward"
  |
  NESTED_LOOP_JOIN_EMBEDDING [a = a]
  leftEmbeddingField: "none"
  rightEmbeddingField: "a2"
  |  |
  |  COLLSCAN [test.explain_md]
  |  direction: "forward"
  |
  COLLSCAN [test.explain_md]
  direction: "forward"
```
```
NESTED_LOOP_JOIN_EMBEDDING [a1.a = a,a2.a = a,a3.a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.explain_md]
  |  direction: "forward"
  |
  NESTED_LOOP_JOIN_EMBEDDING [a2.a = a,a1.a = a]
  leftEmbeddingField: "none"
  rightEmbeddingField: "a3"
  |  |
  |  COLLSCAN [test.explain_md]
  |  direction: "forward"
  |
  NESTED_LOOP_JOIN_EMBEDDING [a = a]
  leftEmbeddingField: "a1"
  rightEmbeddingField: "a2"
  |  |
  |  COLLSCAN [test.explain_md]
  |  direction: "forward"
  |
  COLLSCAN [test.explain_md]
  direction: "forward"
```

### ALL plans, all levels
usedJoinOptimization: true

### Rejected plans (total: 431)
Too many plans, printing only top 3 by cost.
```
NESTED_LOOP_JOIN_EMBEDDING [a2.a = a,a = a,a1.a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "a3"
  |  |
  |  COLLSCAN [test.explain_md]
  |  direction: "forward"
  |
  NESTED_LOOP_JOIN_EMBEDDING [a1.a = a,a = a]
  leftEmbeddingField: "none"
  rightEmbeddingField: "a2"
  |  |
  |  COLLSCAN [test.explain_md]
  |  direction: "forward"
  |
  NESTED_LOOP_JOIN_EMBEDDING [a = a]
  leftEmbeddingField: "a1"
  rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.explain_md]
  |  direction: "forward"
  |
  COLLSCAN [test.explain_md]
  direction: "forward"
```
```
NESTED_LOOP_JOIN_EMBEDDING [a2.a = a,a = a,a1.a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "a3"
  |  |
  |  COLLSCAN [test.explain_md]
  |  direction: "forward"
  |
  NESTED_LOOP_JOIN_EMBEDDING [a = a,a2.a = a]
  leftEmbeddingField: "none"
  rightEmbeddingField: "a1"
  |  |
  |  COLLSCAN [test.explain_md]
  |  direction: "forward"
  |
  NESTED_LOOP_JOIN_EMBEDDING [a = a]
  leftEmbeddingField: "none"
  rightEmbeddingField: "a2"
  |  |
  |  COLLSCAN [test.explain_md]
  |  direction: "forward"
  |
  COLLSCAN [test.explain_md]
  direction: "forward"
```
```
NESTED_LOOP_JOIN_EMBEDDING [a2.a = a,a = a,a1.a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "a3"
  |  |
  |  COLLSCAN [test.explain_md]
  |  direction: "forward"
  |
  NESTED_LOOP_JOIN_EMBEDDING [a = a,a2.a = a]
  leftEmbeddingField: "none"
  rightEmbeddingField: "a1"
  |  |
  |  COLLSCAN [test.explain_md]
  |  direction: "forward"
  |
  NESTED_LOOP_JOIN_EMBEDDING [a = a]
  leftEmbeddingField: "a2"
  rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.explain_md]
  |  direction: "forward"
  |
  COLLSCAN [test.explain_md]
  direction: "forward"
```

