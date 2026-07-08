## 1. Basic example where $lookup subpipeline contains multiple $match stages
### No join opt
### Pipeline
```json
[
	{
		"$lookup" : {
			"from" : "basic_joins_md_foreign1",
			"as" : "x",
			"localField" : "a",
			"foreignField" : "a",
			"pipeline" : [
				{
					"$match" : {
						"d" : {
							"$lt" : 3
						}
					}
				},
				{
					"$match" : {
						"c" : "blah"
					}
				},
				{
					"$match" : {
						"_id" : {
							"$gt" : 0
						}
					}
				}
			]
		}
	},
	{
		"$unwind" : "$x"
	}
]
```
### Results
```json
{ "_id" : 2, "a" : 2, "b" : "bar", "x" : { "_id" : 1, "a" : 2, "c" : "blah", "d" : 2 } }
```
### With bottom-up plan enumeration (left-deep)
usedJoinOptimization: true

```
NESTED_LOOP_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "x"
rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign1]
  filter: { "$and" : [ { "c" : { "$eq" : "blah" } }, { "d" : { "$lt" : 3 } }, { "_id" : { "$gt" : 0 } } ] }
  direction: "forward"
```
### With bottom-up plan enumeration (right-deep)
usedJoinOptimization: true

```
NESTED_LOOP_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "x"
rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign1]
  filter: { "$and" : [ { "c" : { "$eq" : "blah" } }, { "d" : { "$lt" : 3 } }, { "_id" : { "$gt" : 0 } } ] }
  direction: "forward"
```
### With bottom-up plan enumeration (zig-zag)
usedJoinOptimization: true

```
NESTED_LOOP_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "x"
rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign1]
  filter: { "$and" : [ { "c" : { "$eq" : "blah" } }, { "d" : { "$lt" : 3 } }, { "_id" : { "$gt" : 0 } } ] }
  direction: "forward"
```
### With random order, seed 44
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  filter: { "$and" : [ { "c" : { "$eq" : "blah" } }, { "d" : { "$lt" : 3 } }, { "_id" : { "$gt" : 0 } } ] }
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md]
  direction: "forward"
```
### With random order, seed 45
usedJoinOptimization: true

```
NESTED_LOOP_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  filter: { "$and" : [ { "c" : { "$eq" : "blah" } }, { "d" : { "$lt" : 3 } }, { "_id" : { "$gt" : 0 } } ] }
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md]
  direction: "forward"
```
### With random order, index join
usedJoinOptimization: true

```
NESTED_LOOP_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  filter: { "$and" : [ { "c" : { "$eq" : "blah" } }, { "d" : { "$lt" : 3 } }, { "_id" : { "$gt" : 0 } } ] }
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md]
  direction: "forward"
```
### With bottom-up plan enumeration and indexes
usedJoinOptimization: true

```
NESTED_LOOP_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "x"
rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign1]
  filter: { "$and" : [ { "c" : { "$eq" : "blah" } }, { "d" : { "$lt" : 3 } }, { "_id" : { "$gt" : 0 } } ] }
  direction: "forward"
```
## 2. Basic example with two joins
### No join opt
### Pipeline
```json
[
	{
		"$lookup" : {
			"from" : "basic_joins_md_foreign1",
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
			"from" : "basic_joins_md_foreign2",
			"as" : "y",
			"localField" : "b",
			"foreignField" : "b"
		}
	},
	{
		"$unwind" : "$y"
	}
]
```
### Results
```json
{ "_id" : 1, "a" : 1, "b" : "bar", "x" : { "_id" : 0, "a" : 1, "c" : "zoo", "d" : 1 }, "y" : { "_id" : 0, "b" : "bar", "d" : 2 } }
{ "_id" : 1, "a" : 1, "b" : "bar", "x" : { "_id" : 0, "a" : 1, "c" : "zoo", "d" : 1 }, "y" : { "_id" : 1, "b" : "bar", "d" : 6 } }
{ "_id" : 2, "a" : 2, "b" : "bar", "x" : { "_id" : 1, "a" : 2, "c" : "blah", "d" : 2 }, "y" : { "_id" : 0, "b" : "bar", "d" : 2 } }
{ "_id" : 2, "a" : 2, "b" : "bar", "x" : { "_id" : 1, "a" : 2, "c" : "blah", "d" : 2 }, "y" : { "_id" : 1, "b" : "bar", "d" : 6 } }
{ "_id" : 2, "a" : 2, "b" : "bar", "x" : { "_id" : 2, "a" : 2, "c" : "x", "d" : 3 }, "y" : { "_id" : 0, "b" : "bar", "d" : 2 } }
{ "_id" : 2, "a" : 2, "b" : "bar", "x" : { "_id" : 2, "a" : 2, "c" : "x", "d" : 3 }, "y" : { "_id" : 1, "b" : "bar", "d" : 6 } }
{ "_id" : 3, "a" : null, "b" : "bar", "x" : { "_id" : 3, "a" : null, "c" : "x", "d" : 4 }, "y" : { "_id" : 0, "b" : "bar", "d" : 2 } }
{ "_id" : 3, "a" : null, "b" : "bar", "x" : { "_id" : 3, "a" : null, "c" : "x", "d" : 4 }, "y" : { "_id" : 1, "b" : "bar", "d" : 6 } }
{ "_id" : 3, "a" : null, "b" : "bar", "x" : { "_id" : 4, "c" : "x", "d" : 5 }, "y" : { "_id" : 0, "b" : "bar", "d" : 2 } }
{ "_id" : 3, "a" : null, "b" : "bar", "x" : { "_id" : 4, "c" : "x", "d" : 5 }, "y" : { "_id" : 1, "b" : "bar", "d" : 6 } }
{ "_id" : 4, "b" : "bar", "x" : { "_id" : 3, "a" : null, "c" : "x", "d" : 4 }, "y" : { "_id" : 0, "b" : "bar", "d" : 2 } }
{ "_id" : 4, "b" : "bar", "x" : { "_id" : 3, "a" : null, "c" : "x", "d" : 4 }, "y" : { "_id" : 1, "b" : "bar", "d" : 6 } }
{ "_id" : 4, "b" : "bar", "x" : { "_id" : 4, "c" : "x", "d" : 5 }, "y" : { "_id" : 0, "b" : "bar", "d" : 2 } }
{ "_id" : 4, "b" : "bar", "x" : { "_id" : 4, "c" : "x", "d" : 5 }, "y" : { "_id" : 1, "b" : "bar", "d" : 6 } }
```
### With bottom-up plan enumeration (left-deep)
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [b = b]
  leftEmbeddingField: "y"
  rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign2]
  direction: "forward"
```
### With bottom-up plan enumeration (right-deep)
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "x"
rightEmbeddingField: "none"
  |  |
  |  HASH_JOIN_EMBEDDING [b = b]
  |  leftEmbeddingField: "y"
  |  rightEmbeddingField: "none"
  |  |  |
  |  |  COLLSCAN [test.basic_joins_md]
  |  |  direction: "forward"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign2]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign1]
  direction: "forward"
```
### With bottom-up plan enumeration (zig-zag)
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [b = b]
  leftEmbeddingField: "y"
  rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign2]
  direction: "forward"
```
### With random order, seed 44
usedJoinOptimization: true

```
NESTED_LOOP_JOIN_EMBEDDING [b = b]
leftEmbeddingField: "none"
rightEmbeddingField: "y"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign2]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [a = a]
  leftEmbeddingField: "x"
  rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign1]
  direction: "forward"
```
### With random order, seed 45
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  direction: "forward"
  |
  NESTED_LOOP_JOIN_EMBEDDING [b = b]
  leftEmbeddingField: "none"
  rightEmbeddingField: "y"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign2]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md]
  direction: "forward"
```
### With random order, index join
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  direction: "forward"
  |
  NESTED_LOOP_JOIN_EMBEDDING [b = b]
  leftEmbeddingField: "none"
  rightEmbeddingField: "y"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign2]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md]
  direction: "forward"
```
### With bottom-up plan enumeration and indexes
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [b = b]
  leftEmbeddingField: "y"
  rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign2]
  direction: "forward"
```
## 3. Basic example with two joins and suffix
### No join opt
### Pipeline
```json
[
	{
		"$lookup" : {
			"from" : "basic_joins_md_foreign1",
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
			"from" : "basic_joins_md_foreign2",
			"as" : "y",
			"localField" : "b",
			"foreignField" : "b"
		}
	},
	{
		"$unwind" : "$y"
	},
	{
		"$sortByCount" : "$y.b"
	}
]
```
### Results
```json
{ "_id" : "bar", "count" : 14 }
```
### With bottom-up plan enumeration (left-deep)
usedJoinOptimization: true

```
SORT
sortPattern: { "count" : -1 }
memLimit: 104857600
type: "default"
  |
  GROUP
  
  |
  HASH_JOIN_EMBEDDING [a = a]
  leftEmbeddingField: "none"
  rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [b = b]
  leftEmbeddingField: "y"
  rightEmbeddingField: "none"
  |  |
  |  PROJECTION_SIMPLE
  |  transformBy: { "a" : true, "b" : true, "_id" : false }
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign2]
  direction: "forward"
```
### With bottom-up plan enumeration (right-deep)
usedJoinOptimization: true

```
SORT
sortPattern: { "count" : -1 }
memLimit: 104857600
type: "default"
  |
  GROUP
  
  |
  HASH_JOIN_EMBEDDING [a = a]
  leftEmbeddingField: "x"
  rightEmbeddingField: "none"
  |  |
  |  HASH_JOIN_EMBEDDING [b = b]
  |  leftEmbeddingField: "y"
  |  rightEmbeddingField: "none"
  |  |  |
  |  |  PROJECTION_SIMPLE
  |  |  transformBy: { "a" : true, "b" : true, "_id" : false }
  |  |  |
  |  |  COLLSCAN [test.basic_joins_md]
  |  |  direction: "forward"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign2]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign1]
  direction: "forward"
```
### With bottom-up plan enumeration (zig-zag)
usedJoinOptimization: true

```
SORT
sortPattern: { "count" : -1 }
memLimit: 104857600
type: "default"
  |
  GROUP
  
  |
  HASH_JOIN_EMBEDDING [a = a]
  leftEmbeddingField: "none"
  rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [b = b]
  leftEmbeddingField: "y"
  rightEmbeddingField: "none"
  |  |
  |  PROJECTION_SIMPLE
  |  transformBy: { "a" : true, "b" : true, "_id" : false }
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign2]
  direction: "forward"
```
### With random order, seed 44
usedJoinOptimization: true

```
SORT
sortPattern: { "count" : -1 }
memLimit: 104857600
type: "default"
  |
  GROUP
  
  |
  NESTED_LOOP_JOIN_EMBEDDING [b = b]
  leftEmbeddingField: "none"
  rightEmbeddingField: "y"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign2]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [a = a]
  leftEmbeddingField: "x"
  rightEmbeddingField: "none"
  |  |
  |  PROJECTION_SIMPLE
  |  transformBy: { "a" : true, "b" : true, "_id" : false }
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign1]
  direction: "forward"
```
### With random order, seed 45
usedJoinOptimization: true

```
SORT
sortPattern: { "count" : -1 }
memLimit: 104857600
type: "default"
  |
  GROUP
  
  |
  HASH_JOIN_EMBEDDING [a = a]
  leftEmbeddingField: "none"
  rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  direction: "forward"
  |
  NESTED_LOOP_JOIN_EMBEDDING [b = b]
  leftEmbeddingField: "none"
  rightEmbeddingField: "y"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign2]
  |  direction: "forward"
  |
  PROJECTION_SIMPLE
  transformBy: { "a" : true, "b" : true, "_id" : false }
  |
  COLLSCAN [test.basic_joins_md]
  direction: "forward"
```
### With random order, index join
usedJoinOptimization: true

```
SORT
sortPattern: { "count" : -1 }
memLimit: 104857600
type: "default"
  |
  GROUP
  
  |
  HASH_JOIN_EMBEDDING [a = a]
  leftEmbeddingField: "none"
  rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  direction: "forward"
  |
  NESTED_LOOP_JOIN_EMBEDDING [b = b]
  leftEmbeddingField: "none"
  rightEmbeddingField: "y"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign2]
  |  direction: "forward"
  |
  PROJECTION_SIMPLE
  transformBy: { "a" : true, "b" : true, "_id" : false }
  |
  COLLSCAN [test.basic_joins_md]
  direction: "forward"
```
### With bottom-up plan enumeration and indexes
usedJoinOptimization: true

```
SORT
sortPattern: { "count" : -1 }
memLimit: 104857600
type: "default"
  |
  GROUP
  
  |
  HASH_JOIN_EMBEDDING [a = a]
  leftEmbeddingField: "none"
  rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [b = b]
  leftEmbeddingField: "y"
  rightEmbeddingField: "none"
  |  |
  |  PROJECTION_SIMPLE
  |  transformBy: { "a" : true, "b" : true, "_id" : false }
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign2]
  direction: "forward"
```
## 4. Example with two joins, suffix, and sub-pipeline with un-correlated $match
### No join opt
### Pipeline
```json
[
	{
		"$lookup" : {
			"from" : "basic_joins_md_foreign1",
			"as" : "x",
			"localField" : "a",
			"foreignField" : "a",
			"pipeline" : [
				{
					"$match" : {
						"d" : {
							"$lt" : 3
						}
					}
				}
			]
		}
	},
	{
		"$unwind" : "$x"
	},
	{
		"$lookup" : {
			"from" : "basic_joins_md_foreign2",
			"as" : "y",
			"localField" : "b",
			"foreignField" : "b",
			"pipeline" : [
				{
					"$match" : {
						"b" : {
							"$gt" : "aaa"
						}
					}
				}
			]
		}
	},
	{
		"$unwind" : "$y"
	},
	{
		"$sortByCount" : "$x.a"
	}
]
```
### Results
```json
{ "_id" : 1, "count" : 2 }
{ "_id" : 2, "count" : 2 }
```
### With bottom-up plan enumeration (left-deep)
usedJoinOptimization: true

```
SORT
sortPattern: { "count" : -1 }
memLimit: 104857600
type: "default"
  |
  GROUP
  
  |
  HASH_JOIN_EMBEDDING [b = b]
  leftEmbeddingField: "none"
  rightEmbeddingField: "y"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign2]
  |  filter: { "b" : { "$gt" : "aaa" } }
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [a = a]
  leftEmbeddingField: "x"
  rightEmbeddingField: "none"
  |  |
  |  PROJECTION_SIMPLE
  |  transformBy: { "a" : true, "b" : true, "_id" : false }
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign1]
  filter: { "d" : { "$lt" : 3 } }
  direction: "forward"
```
### With bottom-up plan enumeration (right-deep)
usedJoinOptimization: true

```
SORT
sortPattern: { "count" : -1 }
memLimit: 104857600
type: "default"
  |
  GROUP
  
  |
  HASH_JOIN_EMBEDDING [b = b]
  leftEmbeddingField: "y"
  rightEmbeddingField: "none"
  |  |
  |  HASH_JOIN_EMBEDDING [a = a]
  |  leftEmbeddingField: "x"
  |  rightEmbeddingField: "none"
  |  |  |
  |  |  PROJECTION_SIMPLE
  |  |  transformBy: { "a" : true, "b" : true, "_id" : false }
  |  |  |
  |  |  COLLSCAN [test.basic_joins_md]
  |  |  direction: "forward"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  filter: { "d" : { "$lt" : 3 } }
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign2]
  filter: { "b" : { "$gt" : "aaa" } }
  direction: "forward"
```
### With bottom-up plan enumeration (zig-zag)
usedJoinOptimization: true

```
SORT
sortPattern: { "count" : -1 }
memLimit: 104857600
type: "default"
  |
  GROUP
  
  |
  HASH_JOIN_EMBEDDING [b = b]
  leftEmbeddingField: "none"
  rightEmbeddingField: "y"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign2]
  |  filter: { "b" : { "$gt" : "aaa" } }
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [a = a]
  leftEmbeddingField: "x"
  rightEmbeddingField: "none"
  |  |
  |  PROJECTION_SIMPLE
  |  transformBy: { "a" : true, "b" : true, "_id" : false }
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign1]
  filter: { "d" : { "$lt" : 3 } }
  direction: "forward"
```
### With random order, seed 44
usedJoinOptimization: true

```
SORT
sortPattern: { "count" : -1 }
memLimit: 104857600
type: "default"
  |
  GROUP
  
  |
  NESTED_LOOP_JOIN_EMBEDDING [b = b]
  leftEmbeddingField: "none"
  rightEmbeddingField: "y"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign2]
  |  filter: { "b" : { "$gt" : "aaa" } }
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [a = a]
  leftEmbeddingField: "x"
  rightEmbeddingField: "none"
  |  |
  |  PROJECTION_SIMPLE
  |  transformBy: { "a" : true, "b" : true, "_id" : false }
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign1]
  filter: { "d" : { "$lt" : 3 } }
  direction: "forward"
```
### With random order, seed 45
usedJoinOptimization: true

```
SORT
sortPattern: { "count" : -1 }
memLimit: 104857600
type: "default"
  |
  GROUP
  
  |
  HASH_JOIN_EMBEDDING [a = a]
  leftEmbeddingField: "none"
  rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  filter: { "d" : { "$lt" : 3 } }
  |  direction: "forward"
  |
  NESTED_LOOP_JOIN_EMBEDDING [b = b]
  leftEmbeddingField: "none"
  rightEmbeddingField: "y"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign2]
  |  filter: { "b" : { "$gt" : "aaa" } }
  |  direction: "forward"
  |
  PROJECTION_SIMPLE
  transformBy: { "a" : true, "b" : true, "_id" : false }
  |
  COLLSCAN [test.basic_joins_md]
  direction: "forward"
```
### With random order, index join
usedJoinOptimization: true

```
SORT
sortPattern: { "count" : -1 }
memLimit: 104857600
type: "default"
  |
  GROUP
  
  |
  HASH_JOIN_EMBEDDING [a = a]
  leftEmbeddingField: "none"
  rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  filter: { "d" : { "$lt" : 3 } }
  |  direction: "forward"
  |
  NESTED_LOOP_JOIN_EMBEDDING [b = b]
  leftEmbeddingField: "none"
  rightEmbeddingField: "y"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign2]
  |  filter: { "b" : { "$gt" : "aaa" } }
  |  direction: "forward"
  |
  PROJECTION_SIMPLE
  transformBy: { "a" : true, "b" : true, "_id" : false }
  |
  COLLSCAN [test.basic_joins_md]
  direction: "forward"
```
### With bottom-up plan enumeration and indexes
usedJoinOptimization: true

```
SORT
sortPattern: { "count" : -1 }
memLimit: 104857600
type: "default"
  |
  GROUP
  
  |
  HASH_JOIN_EMBEDDING [b = b]
  leftEmbeddingField: "none"
  rightEmbeddingField: "y"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign2]
  |  filter: { "b" : { "$gt" : "aaa" } }
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [a = a]
  leftEmbeddingField: "x"
  rightEmbeddingField: "none"
  |  |
  |  PROJECTION_SIMPLE
  |  transformBy: { "a" : true, "b" : true, "_id" : false }
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign1]
  filter: { "d" : { "$lt" : 3 } }
  direction: "forward"
```
## 5. Example with two joins and sub-pipeline with un-correlated $match
### No join opt
### Pipeline
```json
[
	{
		"$lookup" : {
			"from" : "basic_joins_md_foreign1",
			"as" : "x",
			"localField" : "a",
			"foreignField" : "a",
			"pipeline" : [
				{
					"$match" : {
						"d" : {
							"$lt" : 3
						}
					}
				}
			]
		}
	},
	{
		"$unwind" : "$x"
	},
	{
		"$lookup" : {
			"from" : "basic_joins_md_foreign2",
			"as" : "y",
			"localField" : "b",
			"foreignField" : "b",
			"pipeline" : [
				{
					"$match" : {
						"b" : {
							"$gt" : "aaa"
						}
					}
				}
			]
		}
	},
	{
		"$unwind" : "$y"
	}
]
```
### Results
```json
{ "_id" : 1, "a" : 1, "b" : "bar", "x" : { "_id" : 0, "a" : 1, "c" : "zoo", "d" : 1 }, "y" : { "_id" : 0, "b" : "bar", "d" : 2 } }
{ "_id" : 1, "a" : 1, "b" : "bar", "x" : { "_id" : 0, "a" : 1, "c" : "zoo", "d" : 1 }, "y" : { "_id" : 1, "b" : "bar", "d" : 6 } }
{ "_id" : 2, "a" : 2, "b" : "bar", "x" : { "_id" : 1, "a" : 2, "c" : "blah", "d" : 2 }, "y" : { "_id" : 0, "b" : "bar", "d" : 2 } }
{ "_id" : 2, "a" : 2, "b" : "bar", "x" : { "_id" : 1, "a" : 2, "c" : "blah", "d" : 2 }, "y" : { "_id" : 1, "b" : "bar", "d" : 6 } }
```
### With bottom-up plan enumeration (left-deep)
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [b = b]
leftEmbeddingField: "none"
rightEmbeddingField: "y"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign2]
  |  filter: { "b" : { "$gt" : "aaa" } }
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [a = a]
  leftEmbeddingField: "x"
  rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign1]
  filter: { "d" : { "$lt" : 3 } }
  direction: "forward"
```
### With bottom-up plan enumeration (right-deep)
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [b = b]
leftEmbeddingField: "y"
rightEmbeddingField: "none"
  |  |
  |  HASH_JOIN_EMBEDDING [a = a]
  |  leftEmbeddingField: "x"
  |  rightEmbeddingField: "none"
  |  |  |
  |  |  COLLSCAN [test.basic_joins_md]
  |  |  direction: "forward"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  filter: { "d" : { "$lt" : 3 } }
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign2]
  filter: { "b" : { "$gt" : "aaa" } }
  direction: "forward"
```
### With bottom-up plan enumeration (zig-zag)
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [b = b]
leftEmbeddingField: "none"
rightEmbeddingField: "y"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign2]
  |  filter: { "b" : { "$gt" : "aaa" } }
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [a = a]
  leftEmbeddingField: "x"
  rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign1]
  filter: { "d" : { "$lt" : 3 } }
  direction: "forward"
```
### With random order, seed 44
usedJoinOptimization: true

```
NESTED_LOOP_JOIN_EMBEDDING [b = b]
leftEmbeddingField: "none"
rightEmbeddingField: "y"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign2]
  |  filter: { "b" : { "$gt" : "aaa" } }
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [a = a]
  leftEmbeddingField: "x"
  rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign1]
  filter: { "d" : { "$lt" : 3 } }
  direction: "forward"
```
### With random order, seed 45
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  filter: { "d" : { "$lt" : 3 } }
  |  direction: "forward"
  |
  NESTED_LOOP_JOIN_EMBEDDING [b = b]
  leftEmbeddingField: "none"
  rightEmbeddingField: "y"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign2]
  |  filter: { "b" : { "$gt" : "aaa" } }
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md]
  direction: "forward"
```
### With random order, index join
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  filter: { "d" : { "$lt" : 3 } }
  |  direction: "forward"
  |
  NESTED_LOOP_JOIN_EMBEDDING [b = b]
  leftEmbeddingField: "none"
  rightEmbeddingField: "y"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign2]
  |  filter: { "b" : { "$gt" : "aaa" } }
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md]
  direction: "forward"
```
### With bottom-up plan enumeration and indexes
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [b = b]
leftEmbeddingField: "none"
rightEmbeddingField: "y"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign2]
  |  filter: { "b" : { "$gt" : "aaa" } }
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [a = a]
  leftEmbeddingField: "x"
  rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign1]
  filter: { "d" : { "$lt" : 3 } }
  direction: "forward"
```
## 6. Example with two joins, suffix, and sub-pipeline with un-correlated $match and $match prefix
### No join opt
### Pipeline
```json
[
	{
		"$match" : {
			"a" : {
				"$gt" : 1
			}
		}
	},
	{
		"$lookup" : {
			"from" : "basic_joins_md_foreign1",
			"as" : "x",
			"localField" : "a",
			"foreignField" : "a",
			"pipeline" : [
				{
					"$match" : {
						"d" : {
							"$lt" : 3
						}
					}
				}
			]
		}
	},
	{
		"$unwind" : "$x"
	},
	{
		"$lookup" : {
			"from" : "basic_joins_md_foreign2",
			"as" : "y",
			"localField" : "b",
			"foreignField" : "b",
			"pipeline" : [
				{
					"$match" : {
						"b" : {
							"$gt" : "aaa"
						}
					}
				}
			]
		}
	},
	{
		"$unwind" : "$y"
	},
	{
		"$sortByCount" : "$x.a"
	}
]
```
### Results
```json
{ "_id" : 2, "count" : 2 }
```
### With bottom-up plan enumeration (left-deep)
usedJoinOptimization: true

```
SORT
sortPattern: { "count" : -1 }
memLimit: 104857600
type: "default"
  |
  GROUP
  
  |
  NESTED_LOOP_JOIN_EMBEDDING [b = b]
  leftEmbeddingField: "none"
  rightEmbeddingField: "y"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign2]
  |  filter: { "b" : { "$gt" : "aaa" } }
  |  direction: "forward"
  |
  NESTED_LOOP_JOIN_EMBEDDING [a = a]
  leftEmbeddingField: "none"
  rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  filter: { "d" : { "$lt" : 3 } }
  |  direction: "forward"
  |
  PROJECTION_SIMPLE
  transformBy: { "a" : true, "b" : true, "_id" : false }
  |
  COLLSCAN [test.basic_joins_md]
  filter: { "a" : { "$gt" : 1 } }
  direction: "forward"
```
### With bottom-up plan enumeration (right-deep)
usedJoinOptimization: true

```
SORT
sortPattern: { "count" : -1 }
memLimit: 104857600
type: "default"
  |
  GROUP
  
  |
  HASH_JOIN_EMBEDDING [b = b]
  leftEmbeddingField: "y"
  rightEmbeddingField: "none"
  |  |
  |  NESTED_LOOP_JOIN_EMBEDDING [a = a]
  |  leftEmbeddingField: "none"
  |  rightEmbeddingField: "x"
  |  |  |
  |  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  |  filter: { "d" : { "$lt" : 3 } }
  |  |  direction: "forward"
  |  |
  |  PROJECTION_SIMPLE
  |  transformBy: { "a" : true, "b" : true, "_id" : false }
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  filter: { "a" : { "$gt" : 1 } }
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign2]
  filter: { "b" : { "$gt" : "aaa" } }
  direction: "forward"
```
### With bottom-up plan enumeration (zig-zag)
usedJoinOptimization: true

```
SORT
sortPattern: { "count" : -1 }
memLimit: 104857600
type: "default"
  |
  GROUP
  
  |
  NESTED_LOOP_JOIN_EMBEDDING [b = b]
  leftEmbeddingField: "none"
  rightEmbeddingField: "y"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign2]
  |  filter: { "b" : { "$gt" : "aaa" } }
  |  direction: "forward"
  |
  NESTED_LOOP_JOIN_EMBEDDING [a = a]
  leftEmbeddingField: "none"
  rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  filter: { "d" : { "$lt" : 3 } }
  |  direction: "forward"
  |
  PROJECTION_SIMPLE
  transformBy: { "a" : true, "b" : true, "_id" : false }
  |
  COLLSCAN [test.basic_joins_md]
  filter: { "a" : { "$gt" : 1 } }
  direction: "forward"
```
### With random order, seed 44
usedJoinOptimization: true

```
SORT
sortPattern: { "count" : -1 }
memLimit: 104857600
type: "default"
  |
  GROUP
  
  |
  NESTED_LOOP_JOIN_EMBEDDING [b = b]
  leftEmbeddingField: "none"
  rightEmbeddingField: "y"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign2]
  |  filter: { "b" : { "$gt" : "aaa" } }
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [a = a]
  leftEmbeddingField: "x"
  rightEmbeddingField: "none"
  |  |
  |  PROJECTION_SIMPLE
  |  transformBy: { "a" : true, "b" : true, "_id" : false }
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  filter: { "a" : { "$gt" : 1 } }
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign1]
  filter: { "d" : { "$lt" : 3 } }
  direction: "forward"
```
### With random order, seed 45
usedJoinOptimization: true

```
SORT
sortPattern: { "count" : -1 }
memLimit: 104857600
type: "default"
  |
  GROUP
  
  |
  HASH_JOIN_EMBEDDING [a = a]
  leftEmbeddingField: "none"
  rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  filter: { "d" : { "$lt" : 3 } }
  |  direction: "forward"
  |
  NESTED_LOOP_JOIN_EMBEDDING [b = b]
  leftEmbeddingField: "none"
  rightEmbeddingField: "y"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign2]
  |  filter: { "b" : { "$gt" : "aaa" } }
  |  direction: "forward"
  |
  PROJECTION_SIMPLE
  transformBy: { "a" : true, "b" : true, "_id" : false }
  |
  COLLSCAN [test.basic_joins_md]
  filter: { "a" : { "$gt" : 1 } }
  direction: "forward"
```
### With random order, index join
usedJoinOptimization: true

```
SORT
sortPattern: { "count" : -1 }
memLimit: 104857600
type: "default"
  |
  GROUP
  
  |
  HASH_JOIN_EMBEDDING [a = a]
  leftEmbeddingField: "none"
  rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  filter: { "d" : { "$lt" : 3 } }
  |  direction: "forward"
  |
  NESTED_LOOP_JOIN_EMBEDDING [b = b]
  leftEmbeddingField: "none"
  rightEmbeddingField: "y"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign2]
  |  filter: { "b" : { "$gt" : "aaa" } }
  |  direction: "forward"
  |
  PROJECTION_SIMPLE
  transformBy: { "a" : true, "b" : true, "_id" : false }
  |
  COLLSCAN [test.basic_joins_md]
  filter: { "a" : { "$gt" : 1 } }
  direction: "forward"
```
### With bottom-up plan enumeration and indexes
usedJoinOptimization: true

```
SORT
sortPattern: { "count" : -1 }
memLimit: 104857600
type: "default"
  |
  GROUP
  
  |
  NESTED_LOOP_JOIN_EMBEDDING [b = b]
  leftEmbeddingField: "none"
  rightEmbeddingField: "y"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign2]
  |  filter: { "b" : { "$gt" : "aaa" } }
  |  direction: "forward"
  |
  NESTED_LOOP_JOIN_EMBEDDING [a = a]
  leftEmbeddingField: "none"
  rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  filter: { "d" : { "$lt" : 3 } }
  |  direction: "forward"
  |
  PROJECTION_SIMPLE
  transformBy: { "a" : true, "b" : true, "_id" : false }
  |
  COLLSCAN [test.basic_joins_md]
  filter: { "a" : { "$gt" : 1 } }
  direction: "forward"
```
## 7. Example with two joins and sub-pipeline with un-correlated $match and $match prefix
### No join opt
### Pipeline
```json
[
	{
		"$match" : {
			"a" : {
				"$gt" : 1
			}
		}
	},
	{
		"$lookup" : {
			"from" : "basic_joins_md_foreign1",
			"as" : "x",
			"localField" : "a",
			"foreignField" : "a",
			"pipeline" : [
				{
					"$match" : {
						"d" : {
							"$lt" : 3
						}
					}
				}
			]
		}
	},
	{
		"$unwind" : "$x"
	},
	{
		"$lookup" : {
			"from" : "basic_joins_md_foreign2",
			"as" : "y",
			"localField" : "b",
			"foreignField" : "b",
			"pipeline" : [
				{
					"$match" : {
						"b" : {
							"$gt" : "aaa"
						}
					}
				}
			]
		}
	},
	{
		"$unwind" : "$y"
	}
]
```
### Results
```json
{ "_id" : 2, "a" : 2, "b" : "bar", "x" : { "_id" : 1, "a" : 2, "c" : "blah", "d" : 2 }, "y" : { "_id" : 0, "b" : "bar", "d" : 2 } }
{ "_id" : 2, "a" : 2, "b" : "bar", "x" : { "_id" : 1, "a" : 2, "c" : "blah", "d" : 2 }, "y" : { "_id" : 1, "b" : "bar", "d" : 6 } }
```
### With bottom-up plan enumeration (left-deep)
usedJoinOptimization: true

```
NESTED_LOOP_JOIN_EMBEDDING [b = b]
leftEmbeddingField: "none"
rightEmbeddingField: "y"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign2]
  |  filter: { "b" : { "$gt" : "aaa" } }
  |  direction: "forward"
  |
  NESTED_LOOP_JOIN_EMBEDDING [a = a]
  leftEmbeddingField: "none"
  rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  filter: { "d" : { "$lt" : 3 } }
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md]
  filter: { "a" : { "$gt" : 1 } }
  direction: "forward"
```
### With bottom-up plan enumeration (right-deep)
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [b = b]
leftEmbeddingField: "y"
rightEmbeddingField: "none"
  |  |
  |  NESTED_LOOP_JOIN_EMBEDDING [a = a]
  |  leftEmbeddingField: "none"
  |  rightEmbeddingField: "x"
  |  |  |
  |  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  |  filter: { "d" : { "$lt" : 3 } }
  |  |  direction: "forward"
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  filter: { "a" : { "$gt" : 1 } }
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign2]
  filter: { "b" : { "$gt" : "aaa" } }
  direction: "forward"
```
### With bottom-up plan enumeration (zig-zag)
usedJoinOptimization: true

```
NESTED_LOOP_JOIN_EMBEDDING [b = b]
leftEmbeddingField: "none"
rightEmbeddingField: "y"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign2]
  |  filter: { "b" : { "$gt" : "aaa" } }
  |  direction: "forward"
  |
  NESTED_LOOP_JOIN_EMBEDDING [a = a]
  leftEmbeddingField: "none"
  rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  filter: { "d" : { "$lt" : 3 } }
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md]
  filter: { "a" : { "$gt" : 1 } }
  direction: "forward"
```
### With random order, seed 44
usedJoinOptimization: true

```
NESTED_LOOP_JOIN_EMBEDDING [b = b]
leftEmbeddingField: "none"
rightEmbeddingField: "y"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign2]
  |  filter: { "b" : { "$gt" : "aaa" } }
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [a = a]
  leftEmbeddingField: "x"
  rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  filter: { "a" : { "$gt" : 1 } }
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign1]
  filter: { "d" : { "$lt" : 3 } }
  direction: "forward"
```
### With random order, seed 45
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  filter: { "d" : { "$lt" : 3 } }
  |  direction: "forward"
  |
  NESTED_LOOP_JOIN_EMBEDDING [b = b]
  leftEmbeddingField: "none"
  rightEmbeddingField: "y"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign2]
  |  filter: { "b" : { "$gt" : "aaa" } }
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md]
  filter: { "a" : { "$gt" : 1 } }
  direction: "forward"
```
### With random order, index join
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  filter: { "d" : { "$lt" : 3 } }
  |  direction: "forward"
  |
  NESTED_LOOP_JOIN_EMBEDDING [b = b]
  leftEmbeddingField: "none"
  rightEmbeddingField: "y"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign2]
  |  filter: { "b" : { "$gt" : "aaa" } }
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md]
  filter: { "a" : { "$gt" : 1 } }
  direction: "forward"
```
### With bottom-up plan enumeration and indexes
usedJoinOptimization: true

```
NESTED_LOOP_JOIN_EMBEDDING [b = b]
leftEmbeddingField: "none"
rightEmbeddingField: "y"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign2]
  |  filter: { "b" : { "$gt" : "aaa" } }
  |  direction: "forward"
  |
  NESTED_LOOP_JOIN_EMBEDDING [a = a]
  leftEmbeddingField: "none"
  rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  filter: { "d" : { "$lt" : 3 } }
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md]
  filter: { "a" : { "$gt" : 1 } }
  direction: "forward"
```
## 8. Basic example with referencing field from previous lookup
### No join opt
### Pipeline
```json
[
	{
		"$lookup" : {
			"from" : "basic_joins_md_foreign1",
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
			"from" : "basic_joins_md_foreign3",
			"as" : "z",
			"localField" : "x.c",
			"foreignField" : "c"
		}
	},
	{
		"$unwind" : "$z"
	}
]
```
### Results
```json
{ "_id" : 0, "a" : 1, "b" : "foo", "x" : { "_id" : 0, "a" : 1, "c" : "zoo", "d" : 1 }, "z" : { "_id" : 0, "a" : 1, "c" : "zoo", "d" : 1 } }
{ "_id" : 1, "a" : 1, "b" : "bar", "x" : { "_id" : 0, "a" : 1, "c" : "zoo", "d" : 1 }, "z" : { "_id" : 0, "a" : 1, "c" : "zoo", "d" : 1 } }
{ "_id" : 2, "a" : 2, "b" : "bar", "x" : { "_id" : 1, "a" : 2, "c" : "blah", "d" : 2 }, "z" : { "_id" : 1, "a" : 2, "c" : "blah", "d" : 2 } }
{ "_id" : 2, "a" : 2, "b" : "bar", "x" : { "_id" : 2, "a" : 2, "c" : "x", "d" : 3 }, "z" : { "_id" : 2, "a" : 2, "c" : "x", "d" : 3 } }
{ "_id" : 3, "a" : null, "b" : "bar", "x" : { "_id" : 3, "a" : null, "c" : "x", "d" : 4 }, "z" : { "_id" : 2, "a" : 2, "c" : "x", "d" : 3 } }
{ "_id" : 3, "a" : null, "b" : "bar", "x" : { "_id" : 4, "c" : "x", "d" : 5 }, "z" : { "_id" : 2, "a" : 2, "c" : "x", "d" : 3 } }
{ "_id" : 4, "b" : "bar", "x" : { "_id" : 3, "a" : null, "c" : "x", "d" : 4 }, "z" : { "_id" : 2, "a" : 2, "c" : "x", "d" : 3 } }
{ "_id" : 4, "b" : "bar", "x" : { "_id" : 4, "c" : "x", "d" : 5 }, "z" : { "_id" : 2, "a" : 2, "c" : "x", "d" : 3 } }
```
### With bottom-up plan enumeration (left-deep)
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [x.a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [c = c]
  leftEmbeddingField: "z"
  rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign3]
  direction: "forward"
```
### With bottom-up plan enumeration (right-deep)
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [a = x.a]
leftEmbeddingField: "none"
rightEmbeddingField: "none"
  |  |
  |  HASH_JOIN_EMBEDDING [c = c]
  |  leftEmbeddingField: "z"
  |  rightEmbeddingField: "x"
  |  |  |
  |  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  |  direction: "forward"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign3]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md]
  direction: "forward"
```
### With bottom-up plan enumeration (zig-zag)
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [x.a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [c = c]
  leftEmbeddingField: "z"
  rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign3]
  direction: "forward"
```
### With random order, seed 44
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [c = x.c]
leftEmbeddingField: "z"
rightEmbeddingField: "none"
  |  |
  |  HASH_JOIN_EMBEDDING [a = a]
  |  leftEmbeddingField: "x"
  |  rightEmbeddingField: "none"
  |  |  |
  |  |  COLLSCAN [test.basic_joins_md]
  |  |  direction: "forward"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign3]
  direction: "forward"
```
### With random order, seed 45
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [x.a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  NESTED_LOOP_JOIN_EMBEDDING [c = c]
  leftEmbeddingField: "x"
  rightEmbeddingField: "z"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign3]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign1]
  direction: "forward"
```
### With random order, index join
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [x.a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  NESTED_LOOP_JOIN_EMBEDDING [c = c]
  leftEmbeddingField: "x"
  rightEmbeddingField: "z"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign3]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign1]
  direction: "forward"
```
### With bottom-up plan enumeration and indexes
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [x.a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [c = c]
  leftEmbeddingField: "z"
  rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign3]
  direction: "forward"
```
## 9. Basic example with 3 joins & subsequent join referencing fields from previous lookups
### No join opt
### Pipeline
```json
[
	{
		"$lookup" : {
			"from" : "basic_joins_md_foreign1",
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
			"from" : "basic_joins_md_foreign2",
			"as" : "y",
			"localField" : "b",
			"foreignField" : "b"
		}
	},
	{
		"$unwind" : "$y"
	},
	{
		"$lookup" : {
			"from" : "basic_joins_md_foreign3",
			"as" : "z",
			"localField" : "x.c",
			"foreignField" : "c"
		}
	},
	{
		"$unwind" : "$z"
	}
]
```
### Results
```json
{ "_id" : 1, "a" : 1, "b" : "bar", "x" : { "_id" : 0, "a" : 1, "c" : "zoo", "d" : 1 }, "y" : { "_id" : 0, "b" : "bar", "d" : 2 }, "z" : { "_id" : 0, "a" : 1, "c" : "zoo", "d" : 1 } }
{ "_id" : 1, "a" : 1, "b" : "bar", "x" : { "_id" : 0, "a" : 1, "c" : "zoo", "d" : 1 }, "y" : { "_id" : 1, "b" : "bar", "d" : 6 }, "z" : { "_id" : 0, "a" : 1, "c" : "zoo", "d" : 1 } }
{ "_id" : 2, "a" : 2, "b" : "bar", "x" : { "_id" : 1, "a" : 2, "c" : "blah", "d" : 2 }, "y" : { "_id" : 0, "b" : "bar", "d" : 2 }, "z" : { "_id" : 1, "a" : 2, "c" : "blah", "d" : 2 } }
{ "_id" : 2, "a" : 2, "b" : "bar", "x" : { "_id" : 1, "a" : 2, "c" : "blah", "d" : 2 }, "y" : { "_id" : 1, "b" : "bar", "d" : 6 }, "z" : { "_id" : 1, "a" : 2, "c" : "blah", "d" : 2 } }
{ "_id" : 2, "a" : 2, "b" : "bar", "x" : { "_id" : 2, "a" : 2, "c" : "x", "d" : 3 }, "y" : { "_id" : 0, "b" : "bar", "d" : 2 }, "z" : { "_id" : 2, "a" : 2, "c" : "x", "d" : 3 } }
{ "_id" : 2, "a" : 2, "b" : "bar", "x" : { "_id" : 2, "a" : 2, "c" : "x", "d" : 3 }, "y" : { "_id" : 1, "b" : "bar", "d" : 6 }, "z" : { "_id" : 2, "a" : 2, "c" : "x", "d" : 3 } }
{ "_id" : 3, "a" : null, "b" : "bar", "x" : { "_id" : 3, "a" : null, "c" : "x", "d" : 4 }, "y" : { "_id" : 0, "b" : "bar", "d" : 2 }, "z" : { "_id" : 2, "a" : 2, "c" : "x", "d" : 3 } }
{ "_id" : 3, "a" : null, "b" : "bar", "x" : { "_id" : 3, "a" : null, "c" : "x", "d" : 4 }, "y" : { "_id" : 1, "b" : "bar", "d" : 6 }, "z" : { "_id" : 2, "a" : 2, "c" : "x", "d" : 3 } }
{ "_id" : 3, "a" : null, "b" : "bar", "x" : { "_id" : 4, "c" : "x", "d" : 5 }, "y" : { "_id" : 0, "b" : "bar", "d" : 2 }, "z" : { "_id" : 2, "a" : 2, "c" : "x", "d" : 3 } }
{ "_id" : 3, "a" : null, "b" : "bar", "x" : { "_id" : 4, "c" : "x", "d" : 5 }, "y" : { "_id" : 1, "b" : "bar", "d" : 6 }, "z" : { "_id" : 2, "a" : 2, "c" : "x", "d" : 3 } }
{ "_id" : 4, "b" : "bar", "x" : { "_id" : 3, "a" : null, "c" : "x", "d" : 4 }, "y" : { "_id" : 0, "b" : "bar", "d" : 2 }, "z" : { "_id" : 2, "a" : 2, "c" : "x", "d" : 3 } }
{ "_id" : 4, "b" : "bar", "x" : { "_id" : 3, "a" : null, "c" : "x", "d" : 4 }, "y" : { "_id" : 1, "b" : "bar", "d" : 6 }, "z" : { "_id" : 2, "a" : 2, "c" : "x", "d" : 3 } }
{ "_id" : 4, "b" : "bar", "x" : { "_id" : 4, "c" : "x", "d" : 5 }, "y" : { "_id" : 0, "b" : "bar", "d" : 2 }, "z" : { "_id" : 2, "a" : 2, "c" : "x", "d" : 3 } }
{ "_id" : 4, "b" : "bar", "x" : { "_id" : 4, "c" : "x", "d" : 5 }, "y" : { "_id" : 1, "b" : "bar", "d" : 6 }, "z" : { "_id" : 2, "a" : 2, "c" : "x", "d" : 3 } }
```
### With bottom-up plan enumeration (left-deep)
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [x.c = c]
leftEmbeddingField: "none"
rightEmbeddingField: "z"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign3]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [a = a]
  leftEmbeddingField: "none"
  rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [b = b]
  leftEmbeddingField: "y"
  rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign2]
  direction: "forward"
```
### With bottom-up plan enumeration (right-deep)
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [c = x.c]
leftEmbeddingField: "z"
rightEmbeddingField: "none"
  |  |
  |  HASH_JOIN_EMBEDDING [a = a]
  |  leftEmbeddingField: "x"
  |  rightEmbeddingField: "none"
  |  |  |
  |  |  HASH_JOIN_EMBEDDING [b = b]
  |  |  leftEmbeddingField: "y"
  |  |  rightEmbeddingField: "none"
  |  |  |  |
  |  |  |  COLLSCAN [test.basic_joins_md]
  |  |  |  direction: "forward"
  |  |  |
  |  |  COLLSCAN [test.basic_joins_md_foreign2]
  |  |  direction: "forward"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign3]
  direction: "forward"
```
### With bottom-up plan enumeration (zig-zag)
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [c = x.c]
leftEmbeddingField: "z"
rightEmbeddingField: "none"
  |  |
  |  HASH_JOIN_EMBEDDING [a = a]
  |  leftEmbeddingField: "none"
  |  rightEmbeddingField: "x"
  |  |  |
  |  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  |  direction: "forward"
  |  |
  |  HASH_JOIN_EMBEDDING [b = b]
  |  leftEmbeddingField: "y"
  |  rightEmbeddingField: "none"
  |  |  |
  |  |  COLLSCAN [test.basic_joins_md]
  |  |  direction: "forward"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign2]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign3]
  direction: "forward"
```
### With random order, seed 44
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [b = b]
leftEmbeddingField: "y"
rightEmbeddingField: "none"
  |  |
  |  HASH_JOIN_EMBEDDING [c = x.c]
  |  leftEmbeddingField: "z"
  |  rightEmbeddingField: "none"
  |  |  |
  |  |  HASH_JOIN_EMBEDDING [a = a]
  |  |  leftEmbeddingField: "none"
  |  |  rightEmbeddingField: "x"
  |  |  |  |
  |  |  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  |  |  direction: "forward"
  |  |  |
  |  |  COLLSCAN [test.basic_joins_md]
  |  |  direction: "forward"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign3]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign2]
  direction: "forward"
```
### With random order, seed 45
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [b = b]
leftEmbeddingField: "none"
rightEmbeddingField: "y"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign2]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [x.a = a]
  leftEmbeddingField: "none"
  rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  NESTED_LOOP_JOIN_EMBEDDING [c = c]
  leftEmbeddingField: "x"
  rightEmbeddingField: "z"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign3]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign1]
  direction: "forward"
```
### With random order, index join
usedJoinOptimization: true

```
INDEXED_NESTED_LOOP_JOIN_EMBEDDING [b = b]
leftEmbeddingField: "none"
rightEmbeddingField: "y"
  |  |
  |  FETCH [test.basic_joins_md_foreign2]
  |  
  |  |
  |  INDEX_PROBE_NODE [test.basic_joins_md_foreign2]
  |  keyPattern: { "b" : 1 }
  |  indexName: "b_1"
  |  isMultiKey: false
  |  isUnique: false
  |  isSparse: false
  |  isPartial: false
  |
  HASH_JOIN_EMBEDDING [x.a = a]
  leftEmbeddingField: "none"
  rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  NESTED_LOOP_JOIN_EMBEDDING [c = c]
  leftEmbeddingField: "x"
  rightEmbeddingField: "z"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign3]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign1]
  direction: "forward"
```
### With bottom-up plan enumeration and indexes
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [x.c = c]
leftEmbeddingField: "none"
rightEmbeddingField: "z"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign3]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [a = a]
  leftEmbeddingField: "none"
  rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [b = b]
  leftEmbeddingField: "y"
  rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign2]
  direction: "forward"
```
## 10. Basic example with 3 joins & subsequent join referencing nested paths
### No join opt
### Pipeline
```json
[
	{
		"$lookup" : {
			"from" : "basic_joins_md_foreign1",
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
			"from" : "basic_joins_md_foreign3",
			"as" : "w.y",
			"localField" : "x.c",
			"foreignField" : "c"
		}
	},
	{
		"$unwind" : "$w.y"
	},
	{
		"$lookup" : {
			"from" : "basic_joins_md_foreign2",
			"as" : "k.y.z",
			"localField" : "w.y.d",
			"foreignField" : "d"
		}
	},
	{
		"$unwind" : "$k.y.z"
	}
]
```
### Results
```json
{ "_id" : 2, "a" : 2, "b" : "bar", "k" : { "y" : { "z" : { "_id" : 0, "b" : "bar", "d" : 2 } } }, "w" : { "y" : { "_id" : 1, "a" : 2, "c" : "blah", "d" : 2 } }, "x" : { "_id" : 1, "a" : 2, "c" : "blah", "d" : 2 } }
```
### With bottom-up plan enumeration (left-deep)
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [x.a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [w.y.c = c]
  leftEmbeddingField: "none"
  rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [d = d]
  leftEmbeddingField: "w.y"
  rightEmbeddingField: "k.y.z"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign2]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign3]
  direction: "forward"
```
### With bottom-up plan enumeration (right-deep)
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [a = x.a]
leftEmbeddingField: "none"
rightEmbeddingField: "none"
  |  |
  |  HASH_JOIN_EMBEDDING [c = w.y.c]
  |  leftEmbeddingField: "x"
  |  rightEmbeddingField: "none"
  |  |  |
  |  |  HASH_JOIN_EMBEDDING [d = d]
  |  |  leftEmbeddingField: "w.y"
  |  |  rightEmbeddingField: "k.y.z"
  |  |  |  |
  |  |  |  COLLSCAN [test.basic_joins_md_foreign2]
  |  |  |  direction: "forward"
  |  |  |
  |  |  COLLSCAN [test.basic_joins_md_foreign3]
  |  |  direction: "forward"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md]
  direction: "forward"
```
### With bottom-up plan enumeration (zig-zag)
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [x.a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [w.y.c = c]
  leftEmbeddingField: "none"
  rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [d = d]
  leftEmbeddingField: "w.y"
  rightEmbeddingField: "k.y.z"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign2]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign3]
  direction: "forward"
```
### With random order, seed 44
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [d = w.y.d]
leftEmbeddingField: "k.y.z"
rightEmbeddingField: "none"
  |  |
  |  NESTED_LOOP_JOIN_EMBEDDING [x.c = c]
  |  leftEmbeddingField: "none"
  |  rightEmbeddingField: "w.y"
  |  |  |
  |  |  COLLSCAN [test.basic_joins_md_foreign3]
  |  |  direction: "forward"
  |  |
  |  HASH_JOIN_EMBEDDING [a = a]
  |  leftEmbeddingField: "none"
  |  rightEmbeddingField: "x"
  |  |  |
  |  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  |  direction: "forward"
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign2]
  direction: "forward"
```
### With random order, seed 45
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [x.a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [w.y.c = c]
  leftEmbeddingField: "none"
  rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  direction: "forward"
  |
  NESTED_LOOP_JOIN_EMBEDDING [d = d]
  leftEmbeddingField: "w.y"
  rightEmbeddingField: "k.y.z"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign2]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign3]
  direction: "forward"
```
### With random order, index join
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [x.a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [w.y.c = c]
  leftEmbeddingField: "none"
  rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  direction: "forward"
  |
  NESTED_LOOP_JOIN_EMBEDDING [d = d]
  leftEmbeddingField: "w.y"
  rightEmbeddingField: "k.y.z"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign2]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign3]
  direction: "forward"
```
### With bottom-up plan enumeration and indexes
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [x.a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [w.y.c = c]
  leftEmbeddingField: "none"
  rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [d = d]
  leftEmbeddingField: "w.y"
  rightEmbeddingField: "k.y.z"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign2]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign3]
  direction: "forward"
```
## 11. Basic example with a $project excluding a field from the base collection
### No join opt
### Pipeline
```json
[
	{
		"$project" : {
			"_id" : false
		}
	},
	{
		"$lookup" : {
			"from" : "basic_joins_md_foreign1",
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
			"from" : "basic_joins_md_foreign2",
			"as" : "y",
			"localField" : "b",
			"foreignField" : "b"
		}
	},
	{
		"$unwind" : "$y"
	}
]
```
### Results
```json
{ "a" : 1, "b" : "bar", "x" : { "_id" : 0, "a" : 1, "c" : "zoo", "d" : 1 }, "y" : { "_id" : 0, "b" : "bar", "d" : 2 } }
{ "a" : 1, "b" : "bar", "x" : { "_id" : 0, "a" : 1, "c" : "zoo", "d" : 1 }, "y" : { "_id" : 1, "b" : "bar", "d" : 6 } }
{ "a" : 2, "b" : "bar", "x" : { "_id" : 1, "a" : 2, "c" : "blah", "d" : 2 }, "y" : { "_id" : 0, "b" : "bar", "d" : 2 } }
{ "a" : 2, "b" : "bar", "x" : { "_id" : 1, "a" : 2, "c" : "blah", "d" : 2 }, "y" : { "_id" : 1, "b" : "bar", "d" : 6 } }
{ "a" : 2, "b" : "bar", "x" : { "_id" : 2, "a" : 2, "c" : "x", "d" : 3 }, "y" : { "_id" : 0, "b" : "bar", "d" : 2 } }
{ "a" : 2, "b" : "bar", "x" : { "_id" : 2, "a" : 2, "c" : "x", "d" : 3 }, "y" : { "_id" : 1, "b" : "bar", "d" : 6 } }
{ "a" : null, "b" : "bar", "x" : { "_id" : 3, "a" : null, "c" : "x", "d" : 4 }, "y" : { "_id" : 0, "b" : "bar", "d" : 2 } }
{ "a" : null, "b" : "bar", "x" : { "_id" : 3, "a" : null, "c" : "x", "d" : 4 }, "y" : { "_id" : 1, "b" : "bar", "d" : 6 } }
{ "a" : null, "b" : "bar", "x" : { "_id" : 4, "c" : "x", "d" : 5 }, "y" : { "_id" : 0, "b" : "bar", "d" : 2 } }
{ "a" : null, "b" : "bar", "x" : { "_id" : 4, "c" : "x", "d" : 5 }, "y" : { "_id" : 1, "b" : "bar", "d" : 6 } }
{ "b" : "bar", "x" : { "_id" : 3, "a" : null, "c" : "x", "d" : 4 }, "y" : { "_id" : 0, "b" : "bar", "d" : 2 } }
{ "b" : "bar", "x" : { "_id" : 3, "a" : null, "c" : "x", "d" : 4 }, "y" : { "_id" : 1, "b" : "bar", "d" : 6 } }
{ "b" : "bar", "x" : { "_id" : 4, "c" : "x", "d" : 5 }, "y" : { "_id" : 0, "b" : "bar", "d" : 2 } }
{ "b" : "bar", "x" : { "_id" : 4, "c" : "x", "d" : 5 }, "y" : { "_id" : 1, "b" : "bar", "d" : 6 } }
```
### With bottom-up plan enumeration (left-deep)
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [b = b]
  leftEmbeddingField: "y"
  rightEmbeddingField: "none"
  |  |
  |  PROJECTION_SIMPLE
  |  transformBy: { "_id" : false }
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign2]
  direction: "forward"
```
### With bottom-up plan enumeration (right-deep)
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "x"
rightEmbeddingField: "none"
  |  |
  |  HASH_JOIN_EMBEDDING [b = b]
  |  leftEmbeddingField: "y"
  |  rightEmbeddingField: "none"
  |  |  |
  |  |  PROJECTION_SIMPLE
  |  |  transformBy: { "_id" : false }
  |  |  |
  |  |  COLLSCAN [test.basic_joins_md]
  |  |  direction: "forward"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign2]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign1]
  direction: "forward"
```
### With bottom-up plan enumeration (zig-zag)
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [b = b]
  leftEmbeddingField: "y"
  rightEmbeddingField: "none"
  |  |
  |  PROJECTION_SIMPLE
  |  transformBy: { "_id" : false }
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign2]
  direction: "forward"
```
### With random order, seed 44
usedJoinOptimization: true

```
NESTED_LOOP_JOIN_EMBEDDING [b = b]
leftEmbeddingField: "none"
rightEmbeddingField: "y"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign2]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [a = a]
  leftEmbeddingField: "x"
  rightEmbeddingField: "none"
  |  |
  |  PROJECTION_SIMPLE
  |  transformBy: { "_id" : false }
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign1]
  direction: "forward"
```
### With random order, seed 45
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  direction: "forward"
  |
  NESTED_LOOP_JOIN_EMBEDDING [b = b]
  leftEmbeddingField: "none"
  rightEmbeddingField: "y"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign2]
  |  direction: "forward"
  |
  PROJECTION_SIMPLE
  transformBy: { "_id" : false }
  |
  COLLSCAN [test.basic_joins_md]
  direction: "forward"
```
### With random order, index join
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  direction: "forward"
  |
  NESTED_LOOP_JOIN_EMBEDDING [b = b]
  leftEmbeddingField: "none"
  rightEmbeddingField: "y"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign2]
  |  direction: "forward"
  |
  PROJECTION_SIMPLE
  transformBy: { "_id" : false }
  |
  COLLSCAN [test.basic_joins_md]
  direction: "forward"
```
### With bottom-up plan enumeration and indexes
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [b = b]
  leftEmbeddingField: "y"
  rightEmbeddingField: "none"
  |  |
  |  PROJECTION_SIMPLE
  |  transformBy: { "_id" : false }
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign2]
  direction: "forward"
```
## 12. Example with a cycle in the join graph
### No join opt
### Pipeline
```json
[
	{
		"$match" : {
			"b" : "foo"
		}
	},
	{
		"$lookup" : {
			"from" : "basic_joins_md_foreign1",
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
			"from" : "basic_joins_md_foreign2",
			"as" : "y",
			"localField" : "a",
			"foreignField" : "_id"
		}
	},
	{
		"$unwind" : "$y"
	},
	{
		"$lookup" : {
			"from" : "basic_joins_md_foreign3",
			"as" : "z",
			"localField" : "a",
			"foreignField" : "_id"
		}
	},
	{
		"$unwind" : "$z"
	}
]
```
### Results
```json
{ "_id" : 0, "a" : 1, "b" : "foo", "x" : { "_id" : 0, "a" : 1, "c" : "zoo", "d" : 1 }, "y" : { "_id" : 1, "b" : "bar", "d" : 6 }, "z" : { "_id" : 1, "a" : 2, "c" : "blah", "d" : 2 } }
```
### With bottom-up plan enumeration (left-deep)
usedJoinOptimization: true

```
NESTED_LOOP_JOIN_EMBEDDING [a = a,y._id = a,z._id = a]
leftEmbeddingField: "none"
rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  direction: "forward"
  |
  NESTED_LOOP_JOIN_EMBEDDING [a = _id,y._id = _id]
  leftEmbeddingField: "none"
  rightEmbeddingField: "z"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign3]
  |  direction: "forward"
  |
  NESTED_LOOP_JOIN_EMBEDDING [a = _id]
  leftEmbeddingField: "none"
  rightEmbeddingField: "y"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign2]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md]
  filter: { "b" : { "$eq" : "foo" } }
  direction: "forward"
```
### With bottom-up plan enumeration (right-deep)
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [a = a,a = y._id,a = z._id]
leftEmbeddingField: "x"
rightEmbeddingField: "none"
  |  |
  |  HASH_JOIN_EMBEDDING [_id = a,_id = y._id]
  |  leftEmbeddingField: "z"
  |  rightEmbeddingField: "none"
  |  |  |
  |  |  NESTED_LOOP_JOIN_EMBEDDING [a = _id]
  |  |  leftEmbeddingField: "none"
  |  |  rightEmbeddingField: "y"
  |  |  |  |
  |  |  |  COLLSCAN [test.basic_joins_md_foreign2]
  |  |  |  direction: "forward"
  |  |  |
  |  |  COLLSCAN [test.basic_joins_md]
  |  |  filter: { "b" : { "$eq" : "foo" } }
  |  |  direction: "forward"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign3]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign1]
  direction: "forward"
```
### With bottom-up plan enumeration (zig-zag)
usedJoinOptimization: true

```
NESTED_LOOP_JOIN_EMBEDDING [a = a,y._id = a,z._id = a]
leftEmbeddingField: "none"
rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  direction: "forward"
  |
  NESTED_LOOP_JOIN_EMBEDDING [a = _id,y._id = _id]
  leftEmbeddingField: "none"
  rightEmbeddingField: "z"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign3]
  |  direction: "forward"
  |
  NESTED_LOOP_JOIN_EMBEDDING [a = _id]
  leftEmbeddingField: "none"
  rightEmbeddingField: "y"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign2]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md]
  filter: { "b" : { "$eq" : "foo" } }
  direction: "forward"
```
### With random order, seed 44
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [_id = a,_id = x.a,_id = y._id]
leftEmbeddingField: "z"
rightEmbeddingField: "none"
  |  |
  |  INDEXED_NESTED_LOOP_JOIN_EMBEDDING [a = _id,x.a = _id]
  |  leftEmbeddingField: "none"
  |  rightEmbeddingField: "y"
  |  |  |
  |  |  FETCH [test.basic_joins_md_foreign2]
  |  |  
  |  |  |
  |  |  INDEX_PROBE_NODE [test.basic_joins_md_foreign2]
  |  |  keyPattern: { "_id" : 1 }
  |  |  indexName: "_id_"
  |  |  isMultiKey: false
  |  |  isUnique: true
  |  |  isSparse: false
  |  |  isPartial: false
  |  |
  |  NESTED_LOOP_JOIN_EMBEDDING [a = a]
  |  leftEmbeddingField: "x"
  |  rightEmbeddingField: "none"
  |  |  |
  |  |  COLLSCAN [test.basic_joins_md]
  |  |  filter: { "b" : { "$eq" : "foo" } }
  |  |  direction: "forward"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign3]
  direction: "forward"
```
### With random order, seed 45
usedJoinOptimization: true

```
NESTED_LOOP_JOIN_EMBEDDING [a = a,y._id = a,z._id = a]
leftEmbeddingField: "none"
rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [a = _id,z._id = _id]
  leftEmbeddingField: "none"
  rightEmbeddingField: "y"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign2]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [_id = a]
  leftEmbeddingField: "z"
  rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  filter: { "b" : { "$eq" : "foo" } }
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign3]
  direction: "forward"
```
### With random order, index join
usedJoinOptimization: true

```
NESTED_LOOP_JOIN_EMBEDDING [a = a,y._id = a,z._id = a]
leftEmbeddingField: "none"
rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [a = _id,z._id = _id]
  leftEmbeddingField: "none"
  rightEmbeddingField: "y"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign2]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [_id = a]
  leftEmbeddingField: "z"
  rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  filter: { "b" : { "$eq" : "foo" } }
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign3]
  direction: "forward"
```
### With bottom-up plan enumeration and indexes
usedJoinOptimization: true

```
NESTED_LOOP_JOIN_EMBEDDING [a = a,y._id = a,z._id = a]
leftEmbeddingField: "none"
rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  direction: "forward"
  |
  NESTED_LOOP_JOIN_EMBEDDING [a = _id,y._id = _id]
  leftEmbeddingField: "none"
  rightEmbeddingField: "z"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign3]
  |  direction: "forward"
  |
  NESTED_LOOP_JOIN_EMBEDDING [a = _id]
  leftEmbeddingField: "none"
  rightEmbeddingField: "y"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign2]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md]
  filter: { "b" : { "$eq" : "foo" } }
  direction: "forward"
```
## 13. Basic example with $expr predicates
### No join opt
### Pipeline
```json
[
	{
		"$lookup" : {
			"from" : "basic_joins_md_foreign1",
			"as" : "x",
			"let" : {
				"a" : "$a"
			},
			"pipeline" : [
				{
					"$match" : {
						"$expr" : {
							"$eq" : [
								"$a",
								"$$a"
							]
						}
					}
				}
			]
		}
	},
	{
		"$unwind" : "$x"
	},
	{
		"$lookup" : {
			"from" : "basic_joins_md_foreign2",
			"as" : "z",
			"let" : {
				"b" : "$b"
			},
			"pipeline" : [
				{
					"$match" : {
						"$expr" : {
							"$eq" : [
								"$b",
								"$$b"
							]
						}
					}
				}
			]
		}
	},
	{
		"$unwind" : "$z"
	},
	{
		"$lookup" : {
			"from" : "basic_joins_md_foreign3",
			"as" : "y",
			"localField" : "x.c",
			"foreignField" : "c"
		}
	},
	{
		"$unwind" : "$y"
	}
]
```
### Results
```json
{ "_id" : 1, "a" : 1, "b" : "bar", "x" : { "_id" : 0, "a" : 1, "c" : "zoo", "d" : 1 }, "y" : { "_id" : 0, "a" : 1, "c" : "zoo", "d" : 1 }, "z" : { "_id" : 0, "b" : "bar", "d" : 2 } }
{ "_id" : 1, "a" : 1, "b" : "bar", "x" : { "_id" : 0, "a" : 1, "c" : "zoo", "d" : 1 }, "y" : { "_id" : 0, "a" : 1, "c" : "zoo", "d" : 1 }, "z" : { "_id" : 1, "b" : "bar", "d" : 6 } }
{ "_id" : 2, "a" : 2, "b" : "bar", "x" : { "_id" : 1, "a" : 2, "c" : "blah", "d" : 2 }, "y" : { "_id" : 1, "a" : 2, "c" : "blah", "d" : 2 }, "z" : { "_id" : 0, "b" : "bar", "d" : 2 } }
{ "_id" : 2, "a" : 2, "b" : "bar", "x" : { "_id" : 1, "a" : 2, "c" : "blah", "d" : 2 }, "y" : { "_id" : 1, "a" : 2, "c" : "blah", "d" : 2 }, "z" : { "_id" : 1, "b" : "bar", "d" : 6 } }
{ "_id" : 2, "a" : 2, "b" : "bar", "x" : { "_id" : 2, "a" : 2, "c" : "x", "d" : 3 }, "y" : { "_id" : 2, "a" : 2, "c" : "x", "d" : 3 }, "z" : { "_id" : 0, "b" : "bar", "d" : 2 } }
{ "_id" : 2, "a" : 2, "b" : "bar", "x" : { "_id" : 2, "a" : 2, "c" : "x", "d" : 3 }, "y" : { "_id" : 2, "a" : 2, "c" : "x", "d" : 3 }, "z" : { "_id" : 1, "b" : "bar", "d" : 6 } }
{ "_id" : 3, "a" : null, "b" : "bar", "x" : { "_id" : 3, "a" : null, "c" : "x", "d" : 4 }, "y" : { "_id" : 2, "a" : 2, "c" : "x", "d" : 3 }, "z" : { "_id" : 0, "b" : "bar", "d" : 2 } }
{ "_id" : 3, "a" : null, "b" : "bar", "x" : { "_id" : 3, "a" : null, "c" : "x", "d" : 4 }, "y" : { "_id" : 2, "a" : 2, "c" : "x", "d" : 3 }, "z" : { "_id" : 1, "b" : "bar", "d" : 6 } }
{ "_id" : 4, "b" : "bar", "x" : { "_id" : 4, "c" : "x", "d" : 5 }, "y" : { "_id" : 2, "a" : 2, "c" : "x", "d" : 3 }, "z" : { "_id" : 0, "b" : "bar", "d" : 2 } }
{ "_id" : 4, "b" : "bar", "x" : { "_id" : 4, "c" : "x", "d" : 5 }, "y" : { "_id" : 2, "a" : 2, "c" : "x", "d" : 3 }, "z" : { "_id" : 1, "b" : "bar", "d" : 6 } }
```
### With bottom-up plan enumeration (left-deep)
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [x.c = c]
leftEmbeddingField: "none"
rightEmbeddingField: "y"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign3]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [b $= b]
  leftEmbeddingField: "none"
  rightEmbeddingField: "z"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign2]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [a $= a]
  leftEmbeddingField: "none"
  rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md]
  direction: "forward"
```
### With bottom-up plan enumeration (right-deep)
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [c = x.c]
leftEmbeddingField: "y"
rightEmbeddingField: "none"
  |  |
  |  HASH_JOIN_EMBEDDING [b $= b]
  |  leftEmbeddingField: "z"
  |  rightEmbeddingField: "none"
  |  |  |
  |  |  HASH_JOIN_EMBEDDING [a $= a]
  |  |  leftEmbeddingField: "none"
  |  |  rightEmbeddingField: "x"
  |  |  |  |
  |  |  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  |  |  direction: "forward"
  |  |  |
  |  |  COLLSCAN [test.basic_joins_md]
  |  |  direction: "forward"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign2]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign3]
  direction: "forward"
```
### With bottom-up plan enumeration (zig-zag)
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [c = x.c]
leftEmbeddingField: "y"
rightEmbeddingField: "none"
  |  |
  |  HASH_JOIN_EMBEDDING [b $= b]
  |  leftEmbeddingField: "z"
  |  rightEmbeddingField: "none"
  |  |  |
  |  |  HASH_JOIN_EMBEDDING [a $= a]
  |  |  leftEmbeddingField: "none"
  |  |  rightEmbeddingField: "x"
  |  |  |  |
  |  |  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  |  |  direction: "forward"
  |  |  |
  |  |  COLLSCAN [test.basic_joins_md]
  |  |  direction: "forward"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign2]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign3]
  direction: "forward"
```
### With random order, seed 44
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [b $= b]
leftEmbeddingField: "z"
rightEmbeddingField: "none"
  |  |
  |  HASH_JOIN_EMBEDDING [c = x.c]
  |  leftEmbeddingField: "y"
  |  rightEmbeddingField: "none"
  |  |  |
  |  |  HASH_JOIN_EMBEDDING [a $= a]
  |  |  leftEmbeddingField: "none"
  |  |  rightEmbeddingField: "x"
  |  |  |  |
  |  |  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  |  |  direction: "forward"
  |  |  |
  |  |  COLLSCAN [test.basic_joins_md]
  |  |  direction: "forward"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign3]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign2]
  direction: "forward"
```
### With random order, seed 45
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [b $= b]
leftEmbeddingField: "none"
rightEmbeddingField: "z"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign2]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [x.a $= a]
  leftEmbeddingField: "none"
  rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  NESTED_LOOP_JOIN_EMBEDDING [c = c]
  leftEmbeddingField: "x"
  rightEmbeddingField: "y"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign3]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign1]
  direction: "forward"
```
### With random order, index join
usedJoinOptimization: true

```
INDEXED_NESTED_LOOP_JOIN_EMBEDDING [b $= b]
leftEmbeddingField: "none"
rightEmbeddingField: "z"
  |  |
  |  FETCH [test.basic_joins_md_foreign2]
  |  
  |  |
  |  INDEX_PROBE_NODE [test.basic_joins_md_foreign2]
  |  keyPattern: { "b" : 1 }
  |  indexName: "b_1"
  |  isMultiKey: false
  |  isUnique: false
  |  isSparse: false
  |  isPartial: false
  |
  HASH_JOIN_EMBEDDING [x.a $= a]
  leftEmbeddingField: "none"
  rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  NESTED_LOOP_JOIN_EMBEDDING [c = c]
  leftEmbeddingField: "x"
  rightEmbeddingField: "y"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign3]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign1]
  direction: "forward"
```
### With bottom-up plan enumeration and indexes
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [x.c = c]
leftEmbeddingField: "none"
rightEmbeddingField: "y"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign3]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [b $= b]
  leftEmbeddingField: "none"
  rightEmbeddingField: "z"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign2]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [a $= a]
  leftEmbeddingField: "none"
  rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md]
  direction: "forward"
```
## 14. Example with a $lookup with no join predicate but the rest of the pipeline establishes a connected join graph. 
### No join opt
### Pipeline
```json
[
	{
		"$lookup" : {
			"from" : "basic_joins_md_foreign2",
			"as" : "coll12",
			"pipeline" : [ ]
		}
	},
	{
		"$unwind" : "$coll12"
	},
	{
		"$lookup" : {
			"from" : "basic_joins_md_foreign3",
			"let" : {
				"a" : "$a",
				"a12" : "$coll12.a"
			},
			"pipeline" : [
				{
					"$match" : {
						"$expr" : {
							"$and" : [
								{
									"$eq" : [
										"$a",
										"$$a"
									]
								},
								{
									"$eq" : [
										"$a",
										"$$a12"
									]
								}
							]
						}
					}
				}
			],
			"as" : "coll13"
		}
	},
	{
		"$unwind" : "$coll13"
	}
]
```
### Results
```json

```
### With bottom-up plan enumeration (left-deep)
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [coll13.a $= a,a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "coll12"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign2]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [a $= a]
  leftEmbeddingField: "coll13"
  rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign3]
  direction: "forward"
```
### With bottom-up plan enumeration (right-deep)
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [a $= coll13.a,a = a]
leftEmbeddingField: "coll12"
rightEmbeddingField: "none"
  |  |
  |  HASH_JOIN_EMBEDDING [a $= a]
  |  leftEmbeddingField: "coll13"
  |  rightEmbeddingField: "none"
  |  |  |
  |  |  COLLSCAN [test.basic_joins_md]
  |  |  direction: "forward"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign3]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign2]
  direction: "forward"
```
### With bottom-up plan enumeration (zig-zag)
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [a $= coll13.a,a = a]
leftEmbeddingField: "coll12"
rightEmbeddingField: "none"
  |  |
  |  HASH_JOIN_EMBEDDING [a $= a]
  |  leftEmbeddingField: "coll13"
  |  rightEmbeddingField: "none"
  |  |  |
  |  |  COLLSCAN [test.basic_joins_md]
  |  |  direction: "forward"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign3]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign2]
  direction: "forward"
```
### With random order, seed 44
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [a $= a,a $= coll12.a]
leftEmbeddingField: "coll13"
rightEmbeddingField: "none"
  |  |
  |  HASH_JOIN_EMBEDDING [a = a]
  |  leftEmbeddingField: "coll12"
  |  rightEmbeddingField: "none"
  |  |  |
  |  |  COLLSCAN [test.basic_joins_md]
  |  |  direction: "forward"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign2]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign3]
  direction: "forward"
```
### With random order, seed 45
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [coll13.a $= a,coll12.a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [a $= a]
  leftEmbeddingField: "coll12"
  rightEmbeddingField: "coll13"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign3]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign2]
  direction: "forward"
```
### With random order, index join
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [coll13.a $= a,coll12.a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [a $= a]
  leftEmbeddingField: "coll12"
  rightEmbeddingField: "coll13"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign3]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign2]
  direction: "forward"
```
### With bottom-up plan enumeration and indexes
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [coll13.a $= a,a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "coll12"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign2]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [a $= a]
  leftEmbeddingField: "coll13"
  rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign3]
  direction: "forward"
```
## 15. Projection on ambiguous field
### No join opt
### Pipeline
```json
[
	{
		"$lookup" : {
			"from" : "basic_joins_md_foreign2",
			"as" : "x",
			"localField" : "a",
			"foreignField" : "d"
		}
	},
	{
		"$unwind" : "$x"
	},
	{
		"$project" : {
			"_id" : 0,
			"d" : 1
		}
	}
]
```
### Results
```json
{ }
```
### With bottom-up plan enumeration (left-deep)
usedJoinOptimization: true

```
PROJECTION_DEFAULT
transformBy: { "d" : true, "_id" : false }
  |
  HASH_JOIN_EMBEDDING [d = a]
  leftEmbeddingField: "x"
  rightEmbeddingField: "none"
  |  |
  |  PROJECTION_SIMPLE
  |  transformBy: { "a" : true, "d" : true, "_id" : false }
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign2]
  direction: "forward"
```
### With bottom-up plan enumeration (right-deep)
usedJoinOptimization: true

```
PROJECTION_DEFAULT
transformBy: { "d" : true, "_id" : false }
  |
  HASH_JOIN_EMBEDDING [d = a]
  leftEmbeddingField: "x"
  rightEmbeddingField: "none"
  |  |
  |  PROJECTION_SIMPLE
  |  transformBy: { "a" : true, "d" : true, "_id" : false }
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign2]
  direction: "forward"
```
### With bottom-up plan enumeration (zig-zag)
usedJoinOptimization: true

```
PROJECTION_DEFAULT
transformBy: { "d" : true, "_id" : false }
  |
  HASH_JOIN_EMBEDDING [d = a]
  leftEmbeddingField: "x"
  rightEmbeddingField: "none"
  |  |
  |  PROJECTION_SIMPLE
  |  transformBy: { "a" : true, "d" : true, "_id" : false }
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign2]
  direction: "forward"
```
### With random order, seed 44
usedJoinOptimization: true

```
PROJECTION_DEFAULT
transformBy: { "d" : true, "_id" : false }
  |
  HASH_JOIN_EMBEDDING [a = d]
  leftEmbeddingField: "none"
  rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign2]
  |  direction: "forward"
  |
  PROJECTION_SIMPLE
  transformBy: { "a" : true, "d" : true, "_id" : false }
  |
  COLLSCAN [test.basic_joins_md]
  direction: "forward"
```
### With random order, seed 45
usedJoinOptimization: true

```
PROJECTION_DEFAULT
transformBy: { "d" : true, "_id" : false }
  |
  NESTED_LOOP_JOIN_EMBEDDING [a = d]
  leftEmbeddingField: "none"
  rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign2]
  |  direction: "forward"
  |
  PROJECTION_SIMPLE
  transformBy: { "a" : true, "d" : true, "_id" : false }
  |
  COLLSCAN [test.basic_joins_md]
  direction: "forward"
```
### With random order, index join
usedJoinOptimization: true

```
PROJECTION_DEFAULT
transformBy: { "d" : true, "_id" : false }
  |
  NESTED_LOOP_JOIN_EMBEDDING [a = d]
  leftEmbeddingField: "none"
  rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign2]
  |  direction: "forward"
  |
  PROJECTION_SIMPLE
  transformBy: { "a" : true, "d" : true, "_id" : false }
  |
  COLLSCAN [test.basic_joins_md]
  direction: "forward"
```
### With bottom-up plan enumeration and indexes
usedJoinOptimization: true

```
PROJECTION_DEFAULT
transformBy: { "d" : true, "_id" : false }
  |
  HASH_JOIN_EMBEDDING [d = a]
  leftEmbeddingField: "x"
  rightEmbeddingField: "none"
  |  |
  |  PROJECTION_SIMPLE
  |  transformBy: { "a" : true, "d" : true, "_id" : false }
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign2]
  direction: "forward"
```
## 16. Non-pipeline $lookup with single absorbed $match on as field
### No join opt
### Pipeline
```json
[
	{
		"$lookup" : {
			"from" : "basic_joins_md_foreign1",
			"as" : "x",
			"localField" : "a",
			"foreignField" : "a"
		}
	},
	{
		"$unwind" : "$x"
	},
	{
		"$match" : {
			"x.c" : {
				"$eq" : "blah"
			}
		}
	}
]
```
### Results
```json
{ "_id" : 2, "a" : 2, "b" : "bar", "x" : { "_id" : 1, "a" : 2, "c" : "blah", "d" : 2 } }
```
### With bottom-up plan enumeration (left-deep)
usedJoinOptimization: true

```
NESTED_LOOP_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "x"
rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign1]
  filter: { "c" : { "$eq" : "blah" } }
  direction: "forward"
```
### With bottom-up plan enumeration (right-deep)
usedJoinOptimization: true

```
NESTED_LOOP_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "x"
rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign1]
  filter: { "c" : { "$eq" : "blah" } }
  direction: "forward"
```
### With bottom-up plan enumeration (zig-zag)
usedJoinOptimization: true

```
NESTED_LOOP_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "x"
rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign1]
  filter: { "c" : { "$eq" : "blah" } }
  direction: "forward"
```
### With random order, seed 44
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  filter: { "c" : { "$eq" : "blah" } }
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md]
  direction: "forward"
```
### With random order, seed 45
usedJoinOptimization: true

```
NESTED_LOOP_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  filter: { "c" : { "$eq" : "blah" } }
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md]
  direction: "forward"
```
### With random order, index join
usedJoinOptimization: true

```
NESTED_LOOP_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  filter: { "c" : { "$eq" : "blah" } }
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md]
  direction: "forward"
```
### With bottom-up plan enumeration and indexes
usedJoinOptimization: true

```
NESTED_LOOP_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "x"
rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign1]
  filter: { "c" : { "$eq" : "blah" } }
  direction: "forward"
```
## 17. Non-pipeline $lookup with two absorbed $match stages both on as field
### No join opt
### Pipeline
```json
[
	{
		"$lookup" : {
			"from" : "basic_joins_md_foreign1",
			"as" : "x",
			"localField" : "a",
			"foreignField" : "a"
		}
	},
	{
		"$unwind" : "$x"
	},
	{
		"$match" : {
			"x.c" : {
				"$eq" : "blah"
			}
		}
	},
	{
		"$match" : {
			"x.d" : {
				"$eq" : 2
			}
		}
	}
]
```
### Results
```json
{ "_id" : 2, "a" : 2, "b" : "bar", "x" : { "_id" : 1, "a" : 2, "c" : "blah", "d" : 2 } }
```
### With bottom-up plan enumeration (left-deep)
usedJoinOptimization: true

```
NESTED_LOOP_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "x"
rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign1]
  filter: { "$and" : [ { "c" : { "$eq" : "blah" } }, { "d" : { "$eq" : 2 } } ] }
  direction: "forward"
```
### With bottom-up plan enumeration (right-deep)
usedJoinOptimization: true

```
NESTED_LOOP_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "x"
rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign1]
  filter: { "$and" : [ { "c" : { "$eq" : "blah" } }, { "d" : { "$eq" : 2 } } ] }
  direction: "forward"
```
### With bottom-up plan enumeration (zig-zag)
usedJoinOptimization: true

```
NESTED_LOOP_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "x"
rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign1]
  filter: { "$and" : [ { "c" : { "$eq" : "blah" } }, { "d" : { "$eq" : 2 } } ] }
  direction: "forward"
```
### With random order, seed 44
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  filter: { "$and" : [ { "c" : { "$eq" : "blah" } }, { "d" : { "$eq" : 2 } } ] }
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md]
  direction: "forward"
```
### With random order, seed 45
usedJoinOptimization: true

```
NESTED_LOOP_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  filter: { "$and" : [ { "c" : { "$eq" : "blah" } }, { "d" : { "$eq" : 2 } } ] }
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md]
  direction: "forward"
```
### With random order, index join
usedJoinOptimization: true

```
NESTED_LOOP_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  filter: { "$and" : [ { "c" : { "$eq" : "blah" } }, { "d" : { "$eq" : 2 } } ] }
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md]
  direction: "forward"
```
### With bottom-up plan enumeration and indexes
usedJoinOptimization: true

```
NESTED_LOOP_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "x"
rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign1]
  filter: { "$and" : [ { "c" : { "$eq" : "blah" } }, { "d" : { "$eq" : 2 } } ] }
  direction: "forward"
```
## 18. Non-pipeline $lookup with absorbed $match on as field followed by $match on base field
### No join opt
### Pipeline
```json
[
	{
		"$lookup" : {
			"from" : "basic_joins_md_foreign1",
			"as" : "x",
			"localField" : "a",
			"foreignField" : "a"
		}
	},
	{
		"$unwind" : "$x"
	},
	{
		"$match" : {
			"x.c" : {
				"$eq" : "blah"
			}
		}
	},
	{
		"$match" : {
			"b" : {
				"$eq" : "bar"
			}
		}
	}
]
```
### Results
```json
{ "_id" : 2, "a" : 2, "b" : "bar", "x" : { "_id" : 1, "a" : 2, "c" : "blah", "d" : 2 } }
```
### With bottom-up plan enumeration (left-deep)
usedJoinOptimization: true

```
NESTED_LOOP_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "x"
rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  filter: { "b" : { "$eq" : "bar" } }
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign1]
  filter: { "c" : { "$eq" : "blah" } }
  direction: "forward"
```
### With bottom-up plan enumeration (right-deep)
usedJoinOptimization: true

```
NESTED_LOOP_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "x"
rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  filter: { "b" : { "$eq" : "bar" } }
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign1]
  filter: { "c" : { "$eq" : "blah" } }
  direction: "forward"
```
### With bottom-up plan enumeration (zig-zag)
usedJoinOptimization: true

```
NESTED_LOOP_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "x"
rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  filter: { "b" : { "$eq" : "bar" } }
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign1]
  filter: { "c" : { "$eq" : "blah" } }
  direction: "forward"
```
### With random order, seed 44
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  filter: { "c" : { "$eq" : "blah" } }
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md]
  filter: { "b" : { "$eq" : "bar" } }
  direction: "forward"
```
### With random order, seed 45
usedJoinOptimization: true

```
NESTED_LOOP_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  filter: { "c" : { "$eq" : "blah" } }
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md]
  filter: { "b" : { "$eq" : "bar" } }
  direction: "forward"
```
### With random order, index join
usedJoinOptimization: true

```
NESTED_LOOP_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  filter: { "c" : { "$eq" : "blah" } }
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md]
  filter: { "b" : { "$eq" : "bar" } }
  direction: "forward"
```
### With bottom-up plan enumeration and indexes
usedJoinOptimization: true

```
NESTED_LOOP_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "x"
rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  filter: { "b" : { "$eq" : "bar" } }
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign1]
  filter: { "c" : { "$eq" : "blah" } }
  direction: "forward"
```
## 19. Two joins where second join has absorbed filter
### No join opt
### Pipeline
```json
[
	{
		"$lookup" : {
			"from" : "basic_joins_md_foreign1",
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
			"from" : "basic_joins_md_foreign2",
			"as" : "y",
			"localField" : "b",
			"foreignField" : "b"
		}
	},
	{
		"$unwind" : "$y"
	},
	{
		"$match" : {
			"y.d" : {
				"$gt" : 2
			}
		}
	}
]
```
### Results
```json
{ "_id" : 1, "a" : 1, "b" : "bar", "x" : { "_id" : 0, "a" : 1, "c" : "zoo", "d" : 1 }, "y" : { "_id" : 1, "b" : "bar", "d" : 6 } }
{ "_id" : 2, "a" : 2, "b" : "bar", "x" : { "_id" : 1, "a" : 2, "c" : "blah", "d" : 2 }, "y" : { "_id" : 1, "b" : "bar", "d" : 6 } }
{ "_id" : 2, "a" : 2, "b" : "bar", "x" : { "_id" : 2, "a" : 2, "c" : "x", "d" : 3 }, "y" : { "_id" : 1, "b" : "bar", "d" : 6 } }
{ "_id" : 3, "a" : null, "b" : "bar", "x" : { "_id" : 3, "a" : null, "c" : "x", "d" : 4 }, "y" : { "_id" : 1, "b" : "bar", "d" : 6 } }
{ "_id" : 3, "a" : null, "b" : "bar", "x" : { "_id" : 4, "c" : "x", "d" : 5 }, "y" : { "_id" : 1, "b" : "bar", "d" : 6 } }
{ "_id" : 4, "b" : "bar", "x" : { "_id" : 3, "a" : null, "c" : "x", "d" : 4 }, "y" : { "_id" : 1, "b" : "bar", "d" : 6 } }
{ "_id" : 4, "b" : "bar", "x" : { "_id" : 4, "c" : "x", "d" : 5 }, "y" : { "_id" : 1, "b" : "bar", "d" : 6 } }
```
### With bottom-up plan enumeration (left-deep)
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [b = b]
  leftEmbeddingField: "y"
  rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign2]
  filter: { "d" : { "$gt" : 2 } }
  direction: "forward"
```
### With bottom-up plan enumeration (right-deep)
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "x"
rightEmbeddingField: "none"
  |  |
  |  HASH_JOIN_EMBEDDING [b = b]
  |  leftEmbeddingField: "y"
  |  rightEmbeddingField: "none"
  |  |  |
  |  |  COLLSCAN [test.basic_joins_md]
  |  |  direction: "forward"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign2]
  |  filter: { "d" : { "$gt" : 2 } }
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign1]
  direction: "forward"
```
### With bottom-up plan enumeration (zig-zag)
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [b = b]
  leftEmbeddingField: "y"
  rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign2]
  filter: { "d" : { "$gt" : 2 } }
  direction: "forward"
```
### With random order, seed 44
usedJoinOptimization: true

```
NESTED_LOOP_JOIN_EMBEDDING [b = b]
leftEmbeddingField: "none"
rightEmbeddingField: "y"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign2]
  |  filter: { "d" : { "$gt" : 2 } }
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [a = a]
  leftEmbeddingField: "x"
  rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign1]
  direction: "forward"
```
### With random order, seed 45
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  direction: "forward"
  |
  NESTED_LOOP_JOIN_EMBEDDING [b = b]
  leftEmbeddingField: "none"
  rightEmbeddingField: "y"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign2]
  |  filter: { "d" : { "$gt" : 2 } }
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md]
  direction: "forward"
```
### With random order, index join
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  direction: "forward"
  |
  NESTED_LOOP_JOIN_EMBEDDING [b = b]
  leftEmbeddingField: "none"
  rightEmbeddingField: "y"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign2]
  |  filter: { "d" : { "$gt" : 2 } }
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md]
  direction: "forward"
```
### With bottom-up plan enumeration and indexes
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [b = b]
  leftEmbeddingField: "y"
  rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign2]
  filter: { "d" : { "$gt" : 2 } }
  direction: "forward"
```
## 20. $match referencing the as-field placed before the $lookup that introduces it is a base collection filter and is not absorbed into the joined collection
### No join opt
### Pipeline
```json
[
	{
		"$match" : {
			"x.c" : {
				"$eq" : "blah"
			}
		}
	},
	{
		"$lookup" : {
			"from" : "basic_joins_md_foreign1",
			"as" : "x",
			"localField" : "a",
			"foreignField" : "a"
		}
	},
	{
		"$unwind" : "$x"
	}
]
```
### Results
```json

```
### With bottom-up plan enumeration (left-deep)
usedJoinOptimization: true

```
NESTED_LOOP_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md]
  filter: { "x.c" : { "$eq" : "blah" } }
  direction: "forward"
```
### With bottom-up plan enumeration (right-deep)
usedJoinOptimization: true

```
NESTED_LOOP_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md]
  filter: { "x.c" : { "$eq" : "blah" } }
  direction: "forward"
```
### With bottom-up plan enumeration (zig-zag)
usedJoinOptimization: true

```
NESTED_LOOP_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md]
  filter: { "x.c" : { "$eq" : "blah" } }
  direction: "forward"
```
### With random order, seed 44
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md]
  filter: { "x.c" : { "$eq" : "blah" } }
  direction: "forward"
```
### With random order, seed 45
usedJoinOptimization: true

```
NESTED_LOOP_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md]
  filter: { "x.c" : { "$eq" : "blah" } }
  direction: "forward"
```
### With random order, index join
usedJoinOptimization: true

```
NESTED_LOOP_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md]
  filter: { "x.c" : { "$eq" : "blah" } }
  direction: "forward"
```
### With bottom-up plan enumeration and indexes
usedJoinOptimization: true

```
INDEXED_NESTED_LOOP_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "x"
  |  |
  |  FETCH [test.basic_joins_md_foreign1]
  |  
  |  |
  |  INDEX_PROBE_NODE [test.basic_joins_md_foreign1]
  |  keyPattern: { "a" : 1 }
  |  indexName: "a_1"
  |  isMultiKey: false
  |  isUnique: false
  |  isSparse: false
  |  isPartial: false
  |
  COLLSCAN [test.basic_joins_md]
  filter: { "x.c" : { "$eq" : "blah" } }
  direction: "forward"
```
## 21. Pipeline $lookup with pipeline:[] and absorbed $match on as field
### No join opt
### Pipeline
```json
[
	{
		"$lookup" : {
			"from" : "basic_joins_md_foreign1",
			"as" : "x",
			"localField" : "a",
			"foreignField" : "a",
			"pipeline" : [ ]
		}
	},
	{
		"$unwind" : "$x"
	},
	{
		"$match" : {
			"x.c" : {
				"$eq" : "blah"
			}
		}
	}
]
```
### Results
```json
{ "_id" : 2, "a" : 2, "b" : "bar", "x" : { "_id" : 1, "a" : 2, "c" : "blah", "d" : 2 } }
```
### With bottom-up plan enumeration (left-deep)
usedJoinOptimization: true

```
NESTED_LOOP_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "x"
rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign1]
  filter: { "c" : { "$eq" : "blah" } }
  direction: "forward"
```
### With bottom-up plan enumeration (right-deep)
usedJoinOptimization: true

```
NESTED_LOOP_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "x"
rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign1]
  filter: { "c" : { "$eq" : "blah" } }
  direction: "forward"
```
### With bottom-up plan enumeration (zig-zag)
usedJoinOptimization: true

```
NESTED_LOOP_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "x"
rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign1]
  filter: { "c" : { "$eq" : "blah" } }
  direction: "forward"
```
### With random order, seed 44
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  filter: { "c" : { "$eq" : "blah" } }
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md]
  direction: "forward"
```
### With random order, seed 45
usedJoinOptimization: true

```
NESTED_LOOP_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  filter: { "c" : { "$eq" : "blah" } }
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md]
  direction: "forward"
```
### With random order, index join
usedJoinOptimization: true

```
NESTED_LOOP_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  filter: { "c" : { "$eq" : "blah" } }
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md]
  direction: "forward"
```
### With bottom-up plan enumeration and indexes
usedJoinOptimization: true

```
NESTED_LOOP_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "x"
rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign1]
  filter: { "c" : { "$eq" : "blah" } }
  direction: "forward"
```
## 22. Pipeline $lookup with pipeline:[$match] and absorbed $match on as field
### No join opt
### Pipeline
```json
[
	{
		"$lookup" : {
			"from" : "basic_joins_md_foreign1",
			"as" : "x",
			"localField" : "a",
			"foreignField" : "a",
			"pipeline" : [
				{
					"$match" : {
						"d" : {
							"$lt" : 3
						}
					}
				}
			]
		}
	},
	{
		"$unwind" : "$x"
	},
	{
		"$match" : {
			"x.c" : {
				"$eq" : "blah"
			}
		}
	}
]
```
### Results
```json
{ "_id" : 2, "a" : 2, "b" : "bar", "x" : { "_id" : 1, "a" : 2, "c" : "blah", "d" : 2 } }
```
### With bottom-up plan enumeration (left-deep)
usedJoinOptimization: true

```
NESTED_LOOP_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "x"
rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign1]
  filter: { "$and" : [ { "c" : { "$eq" : "blah" } }, { "d" : { "$lt" : 3 } } ] }
  direction: "forward"
```
### With bottom-up plan enumeration (right-deep)
usedJoinOptimization: true

```
NESTED_LOOP_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "x"
rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign1]
  filter: { "$and" : [ { "c" : { "$eq" : "blah" } }, { "d" : { "$lt" : 3 } } ] }
  direction: "forward"
```
### With bottom-up plan enumeration (zig-zag)
usedJoinOptimization: true

```
NESTED_LOOP_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "x"
rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign1]
  filter: { "$and" : [ { "c" : { "$eq" : "blah" } }, { "d" : { "$lt" : 3 } } ] }
  direction: "forward"
```
### With random order, seed 44
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  filter: { "$and" : [ { "c" : { "$eq" : "blah" } }, { "d" : { "$lt" : 3 } } ] }
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md]
  direction: "forward"
```
### With random order, seed 45
usedJoinOptimization: true

```
NESTED_LOOP_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  filter: { "$and" : [ { "c" : { "$eq" : "blah" } }, { "d" : { "$lt" : 3 } } ] }
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md]
  direction: "forward"
```
### With random order, index join
usedJoinOptimization: true

```
NESTED_LOOP_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  filter: { "$and" : [ { "c" : { "$eq" : "blah" } }, { "d" : { "$lt" : 3 } } ] }
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md]
  direction: "forward"
```
### With bottom-up plan enumeration and indexes
usedJoinOptimization: true

```
NESTED_LOOP_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "x"
rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign1]
  filter: { "$and" : [ { "c" : { "$eq" : "blah" } }, { "d" : { "$lt" : 3 } } ] }
  direction: "forward"
```
## 23. Pipeline $lookup with correlated sub-pipeline and absorbed $match on as field
### No join opt
### Pipeline
```json
[
	{
		"$lookup" : {
			"from" : "basic_joins_md_foreign1",
			"as" : "x",
			"let" : {
				"a" : "$a"
			},
			"pipeline" : [
				{
					"$match" : {
						"$expr" : {
							"$eq" : [
								"$a",
								"$$a"
							]
						}
					}
				}
			]
		}
	},
	{
		"$unwind" : "$x"
	},
	{
		"$match" : {
			"x.c" : {
				"$eq" : "blah"
			}
		}
	}
]
```
### Results
```json
{ "_id" : 2, "a" : 2, "b" : "bar", "x" : { "_id" : 1, "a" : 2, "c" : "blah", "d" : 2 } }
```
### With bottom-up plan enumeration (left-deep)
usedJoinOptimization: true

```
NESTED_LOOP_JOIN_EMBEDDING [a $= a]
leftEmbeddingField: "x"
rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign1]
  filter: { "c" : { "$eq" : "blah" } }
  direction: "forward"
```
### With bottom-up plan enumeration (right-deep)
usedJoinOptimization: true

```
NESTED_LOOP_JOIN_EMBEDDING [a $= a]
leftEmbeddingField: "x"
rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign1]
  filter: { "c" : { "$eq" : "blah" } }
  direction: "forward"
```
### With bottom-up plan enumeration (zig-zag)
usedJoinOptimization: true

```
NESTED_LOOP_JOIN_EMBEDDING [a $= a]
leftEmbeddingField: "x"
rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign1]
  filter: { "c" : { "$eq" : "blah" } }
  direction: "forward"
```
### With random order, seed 44
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [a $= a]
leftEmbeddingField: "none"
rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  filter: { "c" : { "$eq" : "blah" } }
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md]
  direction: "forward"
```
### With random order, seed 45
usedJoinOptimization: true

```
NESTED_LOOP_JOIN_EMBEDDING [a $= a]
leftEmbeddingField: "none"
rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  filter: { "c" : { "$eq" : "blah" } }
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md]
  direction: "forward"
```
### With random order, index join
usedJoinOptimization: true

```
NESTED_LOOP_JOIN_EMBEDDING [a $= a]
leftEmbeddingField: "none"
rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  filter: { "c" : { "$eq" : "blah" } }
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md]
  direction: "forward"
```
### With bottom-up plan enumeration and indexes
usedJoinOptimization: true

```
NESTED_LOOP_JOIN_EMBEDDING [a $= a]
leftEmbeddingField: "x"
rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign1]
  filter: { "c" : { "$eq" : "blah" } }
  direction: "forward"
```
## 24. Basic example with a $project including fields from the base collection
### No join opt
### Pipeline
```json
[
	{
		"$project" : {
			"a" : 1,
			"b" : 1,
			"notUsed" : 1
		}
	},
	{
		"$lookup" : {
			"from" : "basic_joins_md_foreign1",
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
			"from" : "basic_joins_md_foreign2",
			"as" : "y",
			"localField" : "b",
			"foreignField" : "b"
		}
	},
	{
		"$unwind" : "$y"
	}
]
```
### Results
```json
{ "_id" : 1, "a" : 1, "b" : "bar", "x" : { "_id" : 0, "a" : 1, "c" : "zoo", "d" : 1 }, "y" : { "_id" : 0, "b" : "bar", "d" : 2 } }
{ "_id" : 1, "a" : 1, "b" : "bar", "x" : { "_id" : 0, "a" : 1, "c" : "zoo", "d" : 1 }, "y" : { "_id" : 1, "b" : "bar", "d" : 6 } }
{ "_id" : 2, "a" : 2, "b" : "bar", "x" : { "_id" : 1, "a" : 2, "c" : "blah", "d" : 2 }, "y" : { "_id" : 0, "b" : "bar", "d" : 2 } }
{ "_id" : 2, "a" : 2, "b" : "bar", "x" : { "_id" : 1, "a" : 2, "c" : "blah", "d" : 2 }, "y" : { "_id" : 1, "b" : "bar", "d" : 6 } }
{ "_id" : 2, "a" : 2, "b" : "bar", "x" : { "_id" : 2, "a" : 2, "c" : "x", "d" : 3 }, "y" : { "_id" : 0, "b" : "bar", "d" : 2 } }
{ "_id" : 2, "a" : 2, "b" : "bar", "x" : { "_id" : 2, "a" : 2, "c" : "x", "d" : 3 }, "y" : { "_id" : 1, "b" : "bar", "d" : 6 } }
{ "_id" : 3, "a" : null, "b" : "bar", "x" : { "_id" : 3, "a" : null, "c" : "x", "d" : 4 }, "y" : { "_id" : 0, "b" : "bar", "d" : 2 } }
{ "_id" : 3, "a" : null, "b" : "bar", "x" : { "_id" : 3, "a" : null, "c" : "x", "d" : 4 }, "y" : { "_id" : 1, "b" : "bar", "d" : 6 } }
{ "_id" : 3, "a" : null, "b" : "bar", "x" : { "_id" : 4, "c" : "x", "d" : 5 }, "y" : { "_id" : 0, "b" : "bar", "d" : 2 } }
{ "_id" : 3, "a" : null, "b" : "bar", "x" : { "_id" : 4, "c" : "x", "d" : 5 }, "y" : { "_id" : 1, "b" : "bar", "d" : 6 } }
{ "_id" : 4, "b" : "bar", "x" : { "_id" : 3, "a" : null, "c" : "x", "d" : 4 }, "y" : { "_id" : 0, "b" : "bar", "d" : 2 } }
{ "_id" : 4, "b" : "bar", "x" : { "_id" : 3, "a" : null, "c" : "x", "d" : 4 }, "y" : { "_id" : 1, "b" : "bar", "d" : 6 } }
{ "_id" : 4, "b" : "bar", "x" : { "_id" : 4, "c" : "x", "d" : 5 }, "y" : { "_id" : 0, "b" : "bar", "d" : 2 } }
{ "_id" : 4, "b" : "bar", "x" : { "_id" : 4, "c" : "x", "d" : 5 }, "y" : { "_id" : 1, "b" : "bar", "d" : 6 } }
```
### With bottom-up plan enumeration (left-deep)
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [b = b]
  leftEmbeddingField: "y"
  rightEmbeddingField: "none"
  |  |
  |  PROJECTION_SIMPLE
  |  transformBy: { "_id" : true, "a" : true, "b" : true, "notUsed" : true }
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign2]
  direction: "forward"
```
### With bottom-up plan enumeration (right-deep)
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "x"
rightEmbeddingField: "none"
  |  |
  |  HASH_JOIN_EMBEDDING [b = b]
  |  leftEmbeddingField: "y"
  |  rightEmbeddingField: "none"
  |  |  |
  |  |  PROJECTION_SIMPLE
  |  |  transformBy: { "_id" : true, "a" : true, "b" : true, "notUsed" : true }
  |  |  |
  |  |  COLLSCAN [test.basic_joins_md]
  |  |  direction: "forward"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign2]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign1]
  direction: "forward"
```
### With bottom-up plan enumeration (zig-zag)
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [b = b]
  leftEmbeddingField: "y"
  rightEmbeddingField: "none"
  |  |
  |  PROJECTION_SIMPLE
  |  transformBy: { "_id" : true, "a" : true, "b" : true, "notUsed" : true }
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign2]
  direction: "forward"
```
### With random order, seed 44
usedJoinOptimization: true

```
NESTED_LOOP_JOIN_EMBEDDING [b = b]
leftEmbeddingField: "none"
rightEmbeddingField: "y"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign2]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [a = a]
  leftEmbeddingField: "x"
  rightEmbeddingField: "none"
  |  |
  |  PROJECTION_SIMPLE
  |  transformBy: { "_id" : true, "a" : true, "b" : true, "notUsed" : true }
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign1]
  direction: "forward"
```
### With random order, seed 45
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  direction: "forward"
  |
  NESTED_LOOP_JOIN_EMBEDDING [b = b]
  leftEmbeddingField: "none"
  rightEmbeddingField: "y"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign2]
  |  direction: "forward"
  |
  PROJECTION_SIMPLE
  transformBy: { "_id" : true, "a" : true, "b" : true, "notUsed" : true }
  |
  COLLSCAN [test.basic_joins_md]
  direction: "forward"
```
### With random order, index join
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  direction: "forward"
  |
  NESTED_LOOP_JOIN_EMBEDDING [b = b]
  leftEmbeddingField: "none"
  rightEmbeddingField: "y"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign2]
  |  direction: "forward"
  |
  PROJECTION_SIMPLE
  transformBy: { "_id" : true, "a" : true, "b" : true, "notUsed" : true }
  |
  COLLSCAN [test.basic_joins_md]
  direction: "forward"
```
### With bottom-up plan enumeration and indexes
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [b = b]
  leftEmbeddingField: "y"
  rightEmbeddingField: "none"
  |  |
  |  PROJECTION_SIMPLE
  |  transformBy: { "_id" : true, "a" : true, "b" : true, "notUsed" : true }
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md_foreign2]
  direction: "forward"
```
## 25. Basic example with a $project including join-predicate fields from foreign collections
### No join opt
### Pipeline
```json
[
	{
		"$lookup" : {
			"from" : "basic_joins_md_foreign1",
			"as" : "x",
			"localField" : "a",
			"foreignField" : "a",
			"pipeline" : [
				{
					"$project" : {
						"a" : 1
					}
				}
			]
		}
	},
	{
		"$unwind" : "$x"
	},
	{
		"$lookup" : {
			"from" : "basic_joins_md_foreign2",
			"as" : "y",
			"localField" : "b",
			"foreignField" : "b",
			"pipeline" : [
				{
					"$project" : {
						"b" : 1,
						"c" : 1
					}
				}
			]
		}
	},
	{
		"$unwind" : "$y"
	}
]
```
### Results
```json
{ "_id" : 1, "a" : 1, "b" : "bar", "x" : { "_id" : 0, "a" : 1 }, "y" : { "_id" : 0, "b" : "bar" } }
{ "_id" : 1, "a" : 1, "b" : "bar", "x" : { "_id" : 0, "a" : 1 }, "y" : { "_id" : 1, "b" : "bar" } }
{ "_id" : 2, "a" : 2, "b" : "bar", "x" : { "_id" : 1, "a" : 2 }, "y" : { "_id" : 0, "b" : "bar" } }
{ "_id" : 2, "a" : 2, "b" : "bar", "x" : { "_id" : 1, "a" : 2 }, "y" : { "_id" : 1, "b" : "bar" } }
{ "_id" : 2, "a" : 2, "b" : "bar", "x" : { "_id" : 2, "a" : 2 }, "y" : { "_id" : 0, "b" : "bar" } }
{ "_id" : 2, "a" : 2, "b" : "bar", "x" : { "_id" : 2, "a" : 2 }, "y" : { "_id" : 1, "b" : "bar" } }
{ "_id" : 3, "a" : null, "b" : "bar", "x" : { "_id" : 3, "a" : null }, "y" : { "_id" : 0, "b" : "bar" } }
{ "_id" : 3, "a" : null, "b" : "bar", "x" : { "_id" : 3, "a" : null }, "y" : { "_id" : 1, "b" : "bar" } }
{ "_id" : 3, "a" : null, "b" : "bar", "x" : { "_id" : 4 }, "y" : { "_id" : 0, "b" : "bar" } }
{ "_id" : 3, "a" : null, "b" : "bar", "x" : { "_id" : 4 }, "y" : { "_id" : 1, "b" : "bar" } }
{ "_id" : 4, "b" : "bar", "x" : { "_id" : 3, "a" : null }, "y" : { "_id" : 0, "b" : "bar" } }
{ "_id" : 4, "b" : "bar", "x" : { "_id" : 3, "a" : null }, "y" : { "_id" : 1, "b" : "bar" } }
{ "_id" : 4, "b" : "bar", "x" : { "_id" : 4 }, "y" : { "_id" : 0, "b" : "bar" } }
{ "_id" : 4, "b" : "bar", "x" : { "_id" : 4 }, "y" : { "_id" : 1, "b" : "bar" } }
```
### With bottom-up plan enumeration (left-deep)
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "x"
  |  |
  |  PROJECTION_SIMPLE
  |  transformBy: { "_id" : true, "a" : true }
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [b = b]
  leftEmbeddingField: "y"
  rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  PROJECTION_SIMPLE
  transformBy: { "_id" : true, "b" : true, "c" : true }
  |
  COLLSCAN [test.basic_joins_md_foreign2]
  direction: "forward"
```
### With bottom-up plan enumeration (right-deep)
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "x"
rightEmbeddingField: "none"
  |  |
  |  HASH_JOIN_EMBEDDING [b = b]
  |  leftEmbeddingField: "y"
  |  rightEmbeddingField: "none"
  |  |  |
  |  |  COLLSCAN [test.basic_joins_md]
  |  |  direction: "forward"
  |  |
  |  PROJECTION_SIMPLE
  |  transformBy: { "_id" : true, "b" : true, "c" : true }
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign2]
  |  direction: "forward"
  |
  PROJECTION_SIMPLE
  transformBy: { "_id" : true, "a" : true }
  |
  COLLSCAN [test.basic_joins_md_foreign1]
  direction: "forward"
```
### With bottom-up plan enumeration (zig-zag)
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "x"
  |  |
  |  PROJECTION_SIMPLE
  |  transformBy: { "_id" : true, "a" : true }
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [b = b]
  leftEmbeddingField: "y"
  rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  PROJECTION_SIMPLE
  transformBy: { "_id" : true, "b" : true, "c" : true }
  |
  COLLSCAN [test.basic_joins_md_foreign2]
  direction: "forward"
```
### With random order, seed 44
usedJoinOptimization: true

```
NESTED_LOOP_JOIN_EMBEDDING [b = b]
leftEmbeddingField: "none"
rightEmbeddingField: "y"
  |  |
  |  PROJECTION_SIMPLE
  |  transformBy: { "_id" : true, "b" : true, "c" : true }
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign2]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [a = a]
  leftEmbeddingField: "x"
  rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  PROJECTION_SIMPLE
  transformBy: { "_id" : true, "a" : true }
  |
  COLLSCAN [test.basic_joins_md_foreign1]
  direction: "forward"
```
### With random order, seed 45
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "x"
  |  |
  |  PROJECTION_SIMPLE
  |  transformBy: { "_id" : true, "a" : true }
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  direction: "forward"
  |
  NESTED_LOOP_JOIN_EMBEDDING [b = b]
  leftEmbeddingField: "none"
  rightEmbeddingField: "y"
  |  |
  |  PROJECTION_SIMPLE
  |  transformBy: { "_id" : true, "b" : true, "c" : true }
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign2]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md]
  direction: "forward"
```
### With random order, index join
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "x"
  |  |
  |  PROJECTION_SIMPLE
  |  transformBy: { "_id" : true, "a" : true }
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  direction: "forward"
  |
  NESTED_LOOP_JOIN_EMBEDDING [b = b]
  leftEmbeddingField: "none"
  rightEmbeddingField: "y"
  |  |
  |  PROJECTION_SIMPLE
  |  transformBy: { "_id" : true, "b" : true, "c" : true }
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign2]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md]
  direction: "forward"
```
### With bottom-up plan enumeration and indexes
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "x"
  |  |
  |  PROJECTION_SIMPLE
  |  transformBy: { "_id" : true, "a" : true }
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [b = b]
  leftEmbeddingField: "y"
  rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  PROJECTION_SIMPLE
  transformBy: { "_id" : true, "b" : true, "c" : true }
  |
  COLLSCAN [test.basic_joins_md_foreign2]
  direction: "forward"
```
## 26. $project as only stage in subpipeline (no $match), excluding non-join-predicate fields
### No join opt
### Pipeline
```json
[
	{
		"$lookup" : {
			"from" : "basic_joins_md_foreign1",
			"as" : "x",
			"localField" : "a",
			"foreignField" : "a",
			"pipeline" : [
				{
					"$project" : {
						"_id" : 0,
						"c" : 0
					}
				}
			]
		}
	},
	{
		"$unwind" : "$x"
	}
]
```
### Results
```json
{ "_id" : 0, "a" : 1, "b" : "foo", "x" : { "a" : 1, "d" : 1 } }
{ "_id" : 1, "a" : 1, "b" : "bar", "x" : { "a" : 1, "d" : 1 } }
{ "_id" : 2, "a" : 2, "b" : "bar", "x" : { "a" : 2, "d" : 2 } }
{ "_id" : 2, "a" : 2, "b" : "bar", "x" : { "a" : 2, "d" : 3 } }
{ "_id" : 3, "a" : null, "b" : "bar", "x" : { "a" : null, "d" : 4 } }
{ "_id" : 3, "a" : null, "b" : "bar", "x" : { "d" : 5 } }
{ "_id" : 4, "b" : "bar", "x" : { "a" : null, "d" : 4 } }
{ "_id" : 4, "b" : "bar", "x" : { "d" : 5 } }
```
### With bottom-up plan enumeration (left-deep)
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "x"
  |  |
  |  PROJECTION_SIMPLE
  |  transformBy: { "_id" : false, "c" : false }
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md]
  direction: "forward"
```
### With bottom-up plan enumeration (right-deep)
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "x"
  |  |
  |  PROJECTION_SIMPLE
  |  transformBy: { "_id" : false, "c" : false }
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md]
  direction: "forward"
```
### With bottom-up plan enumeration (zig-zag)
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "x"
  |  |
  |  PROJECTION_SIMPLE
  |  transformBy: { "_id" : false, "c" : false }
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md]
  direction: "forward"
```
### With random order, seed 44
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "x"
  |  |
  |  PROJECTION_SIMPLE
  |  transformBy: { "_id" : false, "c" : false }
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md]
  direction: "forward"
```
### With random order, seed 45
usedJoinOptimization: true

```
NESTED_LOOP_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "x"
  |  |
  |  PROJECTION_SIMPLE
  |  transformBy: { "_id" : false, "c" : false }
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md]
  direction: "forward"
```
### With random order, index join
usedJoinOptimization: true

```
NESTED_LOOP_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "x"
  |  |
  |  PROJECTION_SIMPLE
  |  transformBy: { "_id" : false, "c" : false }
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md]
  direction: "forward"
```
### With bottom-up plan enumeration and indexes
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "x"
  |  |
  |  PROJECTION_SIMPLE
  |  transformBy: { "_id" : false, "c" : false }
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md]
  direction: "forward"
```
## 27. $project in prefix excluding a non-join-predicate field with single join
### No join opt
### Pipeline
```json
[
	{
		"$project" : {
			"_id" : 0,
			"b" : 0
		}
	},
	{
		"$lookup" : {
			"from" : "basic_joins_md_foreign1",
			"as" : "x",
			"localField" : "a",
			"foreignField" : "a"
		}
	},
	{
		"$unwind" : "$x"
	}
]
```
### Results
```json
{ "a" : 1, "x" : { "_id" : 0, "a" : 1, "c" : "zoo", "d" : 1 } }
{ "a" : 1, "x" : { "_id" : 0, "a" : 1, "c" : "zoo", "d" : 1 } }
{ "a" : 2, "x" : { "_id" : 1, "a" : 2, "c" : "blah", "d" : 2 } }
{ "a" : 2, "x" : { "_id" : 2, "a" : 2, "c" : "x", "d" : 3 } }
{ "a" : null, "x" : { "_id" : 3, "a" : null, "c" : "x", "d" : 4 } }
{ "a" : null, "x" : { "_id" : 4, "c" : "x", "d" : 5 } }
{ "x" : { "_id" : 3, "a" : null, "c" : "x", "d" : 4 } }
{ "x" : { "_id" : 4, "c" : "x", "d" : 5 } }
```
### With bottom-up plan enumeration (left-deep)
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  direction: "forward"
  |
  PROJECTION_SIMPLE
  transformBy: { "_id" : false, "b" : false }
  |
  COLLSCAN [test.basic_joins_md]
  direction: "forward"
```
### With bottom-up plan enumeration (right-deep)
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  direction: "forward"
  |
  PROJECTION_SIMPLE
  transformBy: { "_id" : false, "b" : false }
  |
  COLLSCAN [test.basic_joins_md]
  direction: "forward"
```
### With bottom-up plan enumeration (zig-zag)
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  direction: "forward"
  |
  PROJECTION_SIMPLE
  transformBy: { "_id" : false, "b" : false }
  |
  COLLSCAN [test.basic_joins_md]
  direction: "forward"
```
### With random order, seed 44
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  direction: "forward"
  |
  PROJECTION_SIMPLE
  transformBy: { "_id" : false, "b" : false }
  |
  COLLSCAN [test.basic_joins_md]
  direction: "forward"
```
### With random order, seed 45
usedJoinOptimization: true

```
NESTED_LOOP_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  direction: "forward"
  |
  PROJECTION_SIMPLE
  transformBy: { "_id" : false, "b" : false }
  |
  COLLSCAN [test.basic_joins_md]
  direction: "forward"
```
### With random order, index join
usedJoinOptimization: true

```
NESTED_LOOP_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  direction: "forward"
  |
  PROJECTION_SIMPLE
  transformBy: { "_id" : false, "b" : false }
  |
  COLLSCAN [test.basic_joins_md]
  direction: "forward"
```
### With bottom-up plan enumeration and indexes
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "x"
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  direction: "forward"
  |
  PROJECTION_SIMPLE
  transformBy: { "_id" : false, "b" : false }
  |
  COLLSCAN [test.basic_joins_md]
  direction: "forward"
```
## 28. Subpipeline with $match followed by multi-field $project excluding non-join fields
### No join opt
### Pipeline
```json
[
	{
		"$lookup" : {
			"from" : "basic_joins_md_foreign1",
			"as" : "x",
			"localField" : "a",
			"foreignField" : "a",
			"pipeline" : [
				{
					"$match" : {
						"d" : {
							"$lt" : 3
						}
					}
				},
				{
					"$project" : {
						"_id" : 0,
						"c" : 0
					}
				}
			]
		}
	},
	{
		"$unwind" : "$x"
	}
]
```
### Results
```json
{ "_id" : 0, "a" : 1, "b" : "foo", "x" : { "a" : 1, "d" : 1 } }
{ "_id" : 1, "a" : 1, "b" : "bar", "x" : { "a" : 1, "d" : 1 } }
{ "_id" : 2, "a" : 2, "b" : "bar", "x" : { "a" : 2, "d" : 2 } }
```
### With bottom-up plan enumeration (left-deep)
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "x"
rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  PROJECTION_SIMPLE
  transformBy: { "_id" : false, "c" : false }
  |
  COLLSCAN [test.basic_joins_md_foreign1]
  filter: { "d" : { "$lt" : 3 } }
  direction: "forward"
```
### With bottom-up plan enumeration (right-deep)
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "x"
rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  PROJECTION_SIMPLE
  transformBy: { "_id" : false, "c" : false }
  |
  COLLSCAN [test.basic_joins_md_foreign1]
  filter: { "d" : { "$lt" : 3 } }
  direction: "forward"
```
### With bottom-up plan enumeration (zig-zag)
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "x"
rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  PROJECTION_SIMPLE
  transformBy: { "_id" : false, "c" : false }
  |
  COLLSCAN [test.basic_joins_md_foreign1]
  filter: { "d" : { "$lt" : 3 } }
  direction: "forward"
```
### With random order, seed 44
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "x"
  |  |
  |  PROJECTION_SIMPLE
  |  transformBy: { "_id" : false, "c" : false }
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  filter: { "d" : { "$lt" : 3 } }
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md]
  direction: "forward"
```
### With random order, seed 45
usedJoinOptimization: true

```
NESTED_LOOP_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "x"
  |  |
  |  PROJECTION_SIMPLE
  |  transformBy: { "_id" : false, "c" : false }
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  filter: { "d" : { "$lt" : 3 } }
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md]
  direction: "forward"
```
### With random order, index join
usedJoinOptimization: true

```
NESTED_LOOP_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "x"
  |  |
  |  PROJECTION_SIMPLE
  |  transformBy: { "_id" : false, "c" : false }
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  filter: { "d" : { "$lt" : 3 } }
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md]
  direction: "forward"
```
### With bottom-up plan enumeration and indexes
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "x"
rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  PROJECTION_SIMPLE
  transformBy: { "_id" : false, "c" : false }
  |
  COLLSCAN [test.basic_joins_md_foreign1]
  filter: { "d" : { "$lt" : 3 } }
  direction: "forward"
```
## 29. Two joins: first with $match and $project subpipeline, second with $project-only subpipeline
### No join opt
### Pipeline
```json
[
	{
		"$lookup" : {
			"from" : "basic_joins_md_foreign1",
			"as" : "x",
			"localField" : "a",
			"foreignField" : "a",
			"pipeline" : [
				{
					"$match" : {
						"d" : {
							"$lt" : 3
						}
					}
				},
				{
					"$project" : {
						"_id" : 0
					}
				}
			]
		}
	},
	{
		"$unwind" : "$x"
	},
	{
		"$lookup" : {
			"from" : "basic_joins_md_foreign2",
			"as" : "y",
			"localField" : "b",
			"foreignField" : "b",
			"pipeline" : [
				{
					"$project" : {
						"_id" : 0,
						"d" : 0
					}
				}
			]
		}
	},
	{
		"$unwind" : "$y"
	}
]
```
### Results
```json
{ "_id" : 1, "a" : 1, "b" : "bar", "x" : { "a" : 1, "c" : "zoo", "d" : 1 }, "y" : { "b" : "bar" } }
{ "_id" : 1, "a" : 1, "b" : "bar", "x" : { "a" : 1, "c" : "zoo", "d" : 1 }, "y" : { "b" : "bar" } }
{ "_id" : 2, "a" : 2, "b" : "bar", "x" : { "a" : 2, "c" : "blah", "d" : 2 }, "y" : { "b" : "bar" } }
{ "_id" : 2, "a" : 2, "b" : "bar", "x" : { "a" : 2, "c" : "blah", "d" : 2 }, "y" : { "b" : "bar" } }
```
### With bottom-up plan enumeration (left-deep)
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [b = b]
leftEmbeddingField: "none"
rightEmbeddingField: "y"
  |  |
  |  PROJECTION_SIMPLE
  |  transformBy: { "_id" : false, "d" : false }
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign2]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [a = a]
  leftEmbeddingField: "x"
  rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  PROJECTION_SIMPLE
  transformBy: { "_id" : false }
  |
  COLLSCAN [test.basic_joins_md_foreign1]
  filter: { "d" : { "$lt" : 3 } }
  direction: "forward"
```
### With bottom-up plan enumeration (right-deep)
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [b = b]
leftEmbeddingField: "y"
rightEmbeddingField: "none"
  |  |
  |  HASH_JOIN_EMBEDDING [a = a]
  |  leftEmbeddingField: "x"
  |  rightEmbeddingField: "none"
  |  |  |
  |  |  COLLSCAN [test.basic_joins_md]
  |  |  direction: "forward"
  |  |
  |  PROJECTION_SIMPLE
  |  transformBy: { "_id" : false }
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  filter: { "d" : { "$lt" : 3 } }
  |  direction: "forward"
  |
  PROJECTION_SIMPLE
  transformBy: { "_id" : false, "d" : false }
  |
  COLLSCAN [test.basic_joins_md_foreign2]
  direction: "forward"
```
### With bottom-up plan enumeration (zig-zag)
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [b = b]
leftEmbeddingField: "none"
rightEmbeddingField: "y"
  |  |
  |  PROJECTION_SIMPLE
  |  transformBy: { "_id" : false, "d" : false }
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign2]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [a = a]
  leftEmbeddingField: "x"
  rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  PROJECTION_SIMPLE
  transformBy: { "_id" : false }
  |
  COLLSCAN [test.basic_joins_md_foreign1]
  filter: { "d" : { "$lt" : 3 } }
  direction: "forward"
```
### With random order, seed 44
usedJoinOptimization: true

```
NESTED_LOOP_JOIN_EMBEDDING [b = b]
leftEmbeddingField: "none"
rightEmbeddingField: "y"
  |  |
  |  PROJECTION_SIMPLE
  |  transformBy: { "_id" : false, "d" : false }
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign2]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [a = a]
  leftEmbeddingField: "x"
  rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  PROJECTION_SIMPLE
  transformBy: { "_id" : false }
  |
  COLLSCAN [test.basic_joins_md_foreign1]
  filter: { "d" : { "$lt" : 3 } }
  direction: "forward"
```
### With random order, seed 45
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "x"
  |  |
  |  PROJECTION_SIMPLE
  |  transformBy: { "_id" : false }
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  filter: { "d" : { "$lt" : 3 } }
  |  direction: "forward"
  |
  NESTED_LOOP_JOIN_EMBEDDING [b = b]
  leftEmbeddingField: "none"
  rightEmbeddingField: "y"
  |  |
  |  PROJECTION_SIMPLE
  |  transformBy: { "_id" : false, "d" : false }
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign2]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md]
  direction: "forward"
```
### With random order, index join
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [a = a]
leftEmbeddingField: "none"
rightEmbeddingField: "x"
  |  |
  |  PROJECTION_SIMPLE
  |  transformBy: { "_id" : false }
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign1]
  |  filter: { "d" : { "$lt" : 3 } }
  |  direction: "forward"
  |
  NESTED_LOOP_JOIN_EMBEDDING [b = b]
  leftEmbeddingField: "none"
  rightEmbeddingField: "y"
  |  |
  |  PROJECTION_SIMPLE
  |  transformBy: { "_id" : false, "d" : false }
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign2]
  |  direction: "forward"
  |
  COLLSCAN [test.basic_joins_md]
  direction: "forward"
```
### With bottom-up plan enumeration and indexes
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [b = b]
leftEmbeddingField: "none"
rightEmbeddingField: "y"
  |  |
  |  PROJECTION_SIMPLE
  |  transformBy: { "_id" : false, "d" : false }
  |  |
  |  COLLSCAN [test.basic_joins_md_foreign2]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [a = a]
  leftEmbeddingField: "x"
  rightEmbeddingField: "none"
  |  |
  |  COLLSCAN [test.basic_joins_md]
  |  direction: "forward"
  |
  PROJECTION_SIMPLE
  transformBy: { "_id" : false }
  |
  COLLSCAN [test.basic_joins_md_foreign1]
  filter: { "d" : { "$lt" : 3 } }
  direction: "forward"
```
