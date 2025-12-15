## 1. 3-Node graph, base node fully connected
### No join opt
### Pipeline
```json
[
	{
		"$lookup" : {
			"from" : "base_coll_reorder_md_a",
			"as" : "x",
			"localField" : "a",
			"foreignField" : "a"
		}
	},
	{
		"$unwind" : "$x"
	},
	{
		"$lookup" : {
			"from" : "base_coll_reorder_md_b",
			"as" : "y",
			"localField" : "b",
			"foreignField" : "b"
		}
	},
	{
		"$unwind" : "$y"
	},
	{
		"$project" : {
			"_id" : 0,
			"x._id" : 0,
			"y._id" : 0
		}
	}
]
```
### Results
```json
{  "a" : 2,  "b" : 3,  "base" : 22,  "x" : {  "a" : 2,  "b" : 3,  "base" : 22 },  "y" : {  "a" : 2,  "b" : 3,  "base" : 22 } }
{  "a" : 2,  "b" : 3,  "base" : 22,  "x" : {  "a" : 2,  "b" : 3,  "base" : 22 },  "y" : {  "a" : 2,  "b" : 3,  "base" : 33 } }
{  "a" : 2,  "b" : 3,  "base" : 3,  "x" : {  "a" : 2,  "b" : 3,  "base" : 22 },  "y" : {  "a" : 2,  "b" : 3,  "base" : 22 } }
{  "a" : 2,  "b" : 3,  "base" : 3,  "x" : {  "a" : 2,  "b" : 3,  "base" : 22 },  "y" : {  "a" : 2,  "b" : 3,  "base" : 33 } }
{  "a" : 2,  "b" : 3,  "base" : 33,  "x" : {  "a" : 2,  "b" : 3,  "base" : 22 },  "y" : {  "a" : 2,  "b" : 3,  "base" : 22 } }
{  "a" : 2,  "b" : 3,  "base" : 33,  "x" : {  "a" : 2,  "b" : 3,  "base" : 22 },  "y" : {  "a" : 2,  "b" : 3,  "base" : 33 } }
```
### Summarized explain
Execution Engine: sbe
```json
{
	"queryShapeHash" : "07EDFA9AFB9088CD25B2C2DC4C13581B00039C20DE4142FB326FE0BB7B7BE349",
	"rejectedPlans" : [ ],
	"winningPlan" : {
		"inputStage" : {
			"asField" : "y",
			"foreignCollection" : "test.base_coll_reorder_md_b",
			"foreignField" : "b",
			"inputStage" : {
				"asField" : "x",
				"foreignCollection" : "test.base_coll_reorder_md_a",
				"foreignField" : "a",
				"inputStage" : {
					"direction" : "forward",
					"filter" : {
						
					},
					"nss" : "test.base_coll_reorder_md_base",
					"stage" : "COLLSCAN"
				},
				"localField" : "a",
				"scanDirection" : "forward",
				"stage" : "EQ_LOOKUP_UNWIND",
				"strategy" : "HashJoin"
			},
			"localField" : "b",
			"scanDirection" : "forward",
			"stage" : "EQ_LOOKUP_UNWIND",
			"strategy" : "HashJoin"
		},
		"planNodeId" : 4,
		"stage" : "PROJECTION_DEFAULT",
		"transformBy" : {
			"_id" : false,
			"x" : {
				"_id" : false
			},
			"y" : {
				"_id" : false
			}
		}
	}
}
```

### Random reordering with seed 0
```
HASH_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.base_coll_reorder_md_a]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [b = b]
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
```
HASH_JOIN_EMBEDDING [b = b]
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
### Random reordering with seed 7
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

## 2. 3-Node graph, base node connected to one node
### No join opt
### Pipeline
```json
[
	{
		"$lookup" : {
			"from" : "base_coll_reorder_md_a",
			"as" : "x",
			"localField" : "a",
			"foreignField" : "a"
		}
	},
	{
		"$unwind" : "$x"
	},
	{
		"$lookup" : {
			"from" : "base_coll_reorder_md_b",
			"as" : "y",
			"localField" : "x.b",
			"foreignField" : "b"
		}
	},
	{
		"$unwind" : "$y"
	},
	{
		"$project" : {
			"_id" : 0,
			"x._id" : 0,
			"y._id" : 0
		}
	}
]
```
### Results
```json
{  "a" : 2,  "b" : -11,  "base" : 22,  "x" : {  "a" : 2,  "b" : 3,  "base" : 22 },  "y" : {  "a" : 2,  "b" : 3,  "base" : 22 } }
{  "a" : 2,  "b" : -11,  "base" : 22,  "x" : {  "a" : 2,  "b" : 3,  "base" : 22 },  "y" : {  "a" : 2,  "b" : 3,  "base" : 33 } }
{  "a" : 2,  "b" : 3,  "base" : 22,  "x" : {  "a" : 2,  "b" : 3,  "base" : 22 },  "y" : {  "a" : 2,  "b" : 3,  "base" : 22 } }
{  "a" : 2,  "b" : 3,  "base" : 22,  "x" : {  "a" : 2,  "b" : 3,  "base" : 22 },  "y" : {  "a" : 2,  "b" : 3,  "base" : 33 } }
{  "a" : 2,  "b" : 3,  "base" : 3,  "x" : {  "a" : 2,  "b" : 3,  "base" : 22 },  "y" : {  "a" : 2,  "b" : 3,  "base" : 22 } }
{  "a" : 2,  "b" : 3,  "base" : 3,  "x" : {  "a" : 2,  "b" : 3,  "base" : 22 },  "y" : {  "a" : 2,  "b" : 3,  "base" : 33 } }
{  "a" : 2,  "b" : 3,  "base" : 33,  "x" : {  "a" : 2,  "b" : 3,  "base" : 22 },  "y" : {  "a" : 2,  "b" : 3,  "base" : 22 } }
{  "a" : 2,  "b" : 3,  "base" : 33,  "x" : {  "a" : 2,  "b" : 3,  "base" : 22 },  "y" : {  "a" : 2,  "b" : 3,  "base" : 33 } }
```
### Summarized explain
Execution Engine: sbe
```json
{
	"queryShapeHash" : "3E65060EE706660AF2949B94879998C4ABE285431BAD8C0BC3E030F868A3CD8B",
	"rejectedPlans" : [ ],
	"winningPlan" : {
		"inputStage" : {
			"asField" : "y",
			"foreignCollection" : "test.base_coll_reorder_md_b",
			"foreignField" : "b",
			"inputStage" : {
				"asField" : "x",
				"foreignCollection" : "test.base_coll_reorder_md_a",
				"foreignField" : "a",
				"inputStage" : {
					"direction" : "forward",
					"filter" : {
						
					},
					"nss" : "test.base_coll_reorder_md_base",
					"stage" : "COLLSCAN"
				},
				"localField" : "a",
				"scanDirection" : "forward",
				"stage" : "EQ_LOOKUP_UNWIND",
				"strategy" : "HashJoin"
			},
			"localField" : "x.b",
			"scanDirection" : "forward",
			"stage" : "EQ_LOOKUP_UNWIND",
			"strategy" : "HashJoin"
		},
		"planNodeId" : 4,
		"stage" : "PROJECTION_DEFAULT",
		"transformBy" : {
			"_id" : false,
			"x" : {
				"_id" : false
			},
			"y" : {
				"_id" : false
			}
		}
	}
}
```

### Random reordering with seed 0
```
HASH_JOIN_EMBEDDING [x.a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.base_coll_reorder_md_base]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [b = b]
  leftEmbeddingField: "y"
  rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.base_coll_reorder_md_a]
  |  direction: "forward"
  |
  COLLSCAN [test.base_coll_reorder_md_b]
  direction: "forward"
```
### Random reordering with seed 1
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
### Random reordering with seed 3
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

## 3. 3-Node graph + potentially inferred edge
### No join opt
### Pipeline
```json
[
	{
		"$lookup" : {
			"from" : "base_coll_reorder_md_a",
			"as" : "x",
			"localField" : "base",
			"foreignField" : "base"
		}
	},
	{
		"$unwind" : "$x"
	},
	{
		"$lookup" : {
			"from" : "base_coll_reorder_md_b",
			"as" : "y",
			"localField" : "base",
			"foreignField" : "base"
		}
	},
	{
		"$unwind" : "$y"
	},
	{
		"$project" : {
			"_id" : 0,
			"x._id" : 0,
			"y._id" : 0
		}
	}
]
```
### Results
```json
{  "a" : 2,  "b" : -11,  "base" : 22,  "x" : {  "a" : 2,  "b" : 3,  "base" : 22 },  "y" : {  "a" : 2,  "b" : 3,  "base" : 22 } }
{  "a" : 2,  "b" : 3,  "base" : 22,  "x" : {  "a" : 2,  "b" : 3,  "base" : 22 },  "y" : {  "a" : 2,  "b" : 3,  "base" : 22 } }
{  "a" : 2,  "b" : 3,  "base" : 33,  "x" : {  "a" : -1,  "b" : -1,  "base" : 33 },  "y" : {  "a" : 2,  "b" : 3,  "base" : 33 } }
```
### Summarized explain
Execution Engine: sbe
```json
{
	"queryShapeHash" : "38C2ABCABF327D1D7292A8A20CEC74497DDA9AE37D1067EDF51ED3B16B663A41",
	"rejectedPlans" : [ ],
	"winningPlan" : {
		"inputStage" : {
			"asField" : "y",
			"foreignCollection" : "test.base_coll_reorder_md_b",
			"foreignField" : "base",
			"inputStage" : {
				"asField" : "x",
				"foreignCollection" : "test.base_coll_reorder_md_a",
				"foreignField" : "base",
				"inputStage" : {
					"direction" : "forward",
					"filter" : {
						
					},
					"nss" : "test.base_coll_reorder_md_base",
					"stage" : "COLLSCAN"
				},
				"localField" : "base",
				"scanDirection" : "forward",
				"stage" : "EQ_LOOKUP_UNWIND",
				"strategy" : "HashJoin"
			},
			"localField" : "base",
			"scanDirection" : "forward",
			"stage" : "EQ_LOOKUP_UNWIND",
			"strategy" : "HashJoin"
		},
		"planNodeId" : 4,
		"stage" : "PROJECTION_DEFAULT",
		"transformBy" : {
			"_id" : false,
			"x" : {
				"_id" : false
			},
			"y" : {
				"_id" : false
			}
		}
	}
}
```

### Random reordering with seed 0
```
HASH_JOIN_EMBEDDING [base = base]
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
```
HASH_JOIN_EMBEDDING [base = base]
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
```
HASH_JOIN_EMBEDDING [base = base]
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
### Random reordering with seed 7
```
HASH_JOIN_EMBEDDING [base = base]
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

