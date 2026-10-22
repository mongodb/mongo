## 1. Syntactic version 1
### Pipeline
```json
[
	{
		"$lookup" : {
			"from" : "syntactic_order_md_grande",
			"localField" : "i_idx",
			"foreignField" : "i_idx",
			"as" : "a"
		}
	},
	{
		"$unwind" : "$a"
	},
	{
		"$lookup" : {
			"from" : "syntactic_order_md_many_rows",
			"localField" : "i_idx",
			"foreignField" : "i_idx",
			"as" : "b"
		}
	},
	{
		"$unwind" : "$b"
	}
]
```
### Results
```json
{ "_id" : 0, "a" : { "_id" : 1, "i_idx" : 1 }, "b" : { "_id" : 1, "i_idx" : 1 }, "i_idx" : 1 }
```
### Join order
```json
(INLJ {a.i_idx = i_idx} _ = (INLJ {i_idx = i_idx} _ = COLLSCAN [test.syntactic_order_md_tiny], a = FETCH [test.syntactic_order_md_grande]), b = FETCH [test.syntactic_order_md_many_rows])
```
### Syntactic version 1 + Join Optimization
usedJoinOptimization: true

```
INDEXED_NESTED_LOOP_JOIN_EMBEDDING [i_idx = i_idx,a.i_idx = i_idx]
leftEmbeddingField: "none"
rightEmbeddingField: "b"
probePredicates: [ "a.i_idx = i_idx" ]
  |  |
  |  FETCH [test.syntactic_order_md_many_rows]
  |  
  |  |
  |  INDEX_PROBE_NODE [test.syntactic_order_md_many_rows]
  |  keyPattern: { "i_idx" : 1 }
  |  indexName: "i_idx_1"
  |  isMultiKey: false
  |  isUnique: false
  |  isSparse: false
  |  isPartial: false
  |
  INDEXED_NESTED_LOOP_JOIN_EMBEDDING [i_idx = i_idx]
  leftEmbeddingField: "none"
  rightEmbeddingField: "a"
  probePredicates: [ "i_idx = i_idx" ]
  |  |
  |  FETCH [test.syntactic_order_md_grande]
  |  
  |  |
  |  INDEX_PROBE_NODE [test.syntactic_order_md_grande]
  |  keyPattern: { "i_idx" : 1 }
  |  indexName: "i_idx_1"
  |  isMultiKey: false
  |  isUnique: false
  |  isSparse: false
  |  isPartial: false
  |
  COLLSCAN [test.syntactic_order_md_tiny]
  direction: "forward"
```
## 2. Syntactic version 2
### Pipeline
```json
[
	{
		"$lookup" : {
			"from" : "syntactic_order_md_tiny",
			"localField" : "i_idx",
			"foreignField" : "i_idx",
			"as" : "b"
		}
	},
	{
		"$unwind" : "$b"
	},
	{
		"$lookup" : {
			"from" : "syntactic_order_md_grande",
			"localField" : "i_idx",
			"foreignField" : "i_idx",
			"as" : "a"
		}
	},
	{
		"$unwind" : "$a"
	}
]
```
### Results
```json
{ "_id" : 1, "a" : { "_id" : 1, "i_idx" : 1 }, "b" : { "_id" : 0, "i_idx" : 1 }, "i_idx" : 1 }
```
### Join order
```json
(INLJ {a.i_idx = i_idx} _ = (INLJ {i_idx = i_idx} b = COLLSCAN [test.syntactic_order_md_tiny], a = FETCH [test.syntactic_order_md_grande]), _ = FETCH [test.syntactic_order_md_many_rows])
```
### Syntactic version 2 + Join Optimization
usedJoinOptimization: true

```
INDEXED_NESTED_LOOP_JOIN_EMBEDDING [b.i_idx = i_idx,a.i_idx = i_idx]
leftEmbeddingField: "none"
rightEmbeddingField: "none"
probePredicates: [ "a.i_idx = i_idx" ]
  |  |
  |  FETCH [test.syntactic_order_md_many_rows]
  |  
  |  |
  |  INDEX_PROBE_NODE [test.syntactic_order_md_many_rows]
  |  keyPattern: { "i_idx" : 1 }
  |  indexName: "i_idx_1"
  |  isMultiKey: false
  |  isUnique: false
  |  isSparse: false
  |  isPartial: false
  |
  INDEXED_NESTED_LOOP_JOIN_EMBEDDING [i_idx = i_idx]
  leftEmbeddingField: "b"
  rightEmbeddingField: "a"
  probePredicates: [ "i_idx = i_idx" ]
  |  |
  |  FETCH [test.syntactic_order_md_grande]
  |  
  |  |
  |  INDEX_PROBE_NODE [test.syntactic_order_md_grande]
  |  keyPattern: { "i_idx" : 1 }
  |  indexName: "i_idx_1"
  |  isMultiKey: false
  |  isUnique: false
  |  isSparse: false
  |  isPartial: false
  |
  COLLSCAN [test.syntactic_order_md_tiny]
  direction: "forward"
```
