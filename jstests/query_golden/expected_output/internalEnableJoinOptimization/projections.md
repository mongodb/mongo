## 1. 2-Nodes, Simple rename join preds (local/foreign)
### Pipeline
```json
[
	{
		"$project" : {
			"x" : "$a"
		}
	},
	{
		"$lookup" : {
			"from" : "projections_md_b",
			"localField" : "x",
			"foreignField" : "b",
			"as" : "j1",
			"pipeline" : [
				{
					"$project" : {
						"y" : "$b"
					}
				}
			]
		}
	},
	{
		"$unwind" : "$j1"
	}
]
```
### Results
```json
{ "_id" : 1, "j1" : { "_id" : 1, "y" : 1 }, "x" : 1 }
```
### 2-Nodes, Simple rename join preds (local/foreign) + Join Optimization
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [y = x]
leftEmbeddingField: "j1"
rightEmbeddingField: "none"
  |  |
  |  PROJECTION_DEFAULT
  |  transformBy: { "_id" : true, "x" : "$a" }
  |  |
  |  COLLSCAN [test.projections_md_a]
  |  direction: "forward"
  |
  PROJECTION_DEFAULT
  transformBy: { "_id" : true, "y" : "$b" }
  |
  COLLSCAN [test.projections_md_b]
  direction: "forward"
```
## 2. 2-Nodes, Simple rename join preds (subpipeline $match)
### Pipeline
```json
[
	{
		"$project" : {
			"x" : "$a"
		}
	},
	{
		"$lookup" : {
			"from" : "projections_md_b",
			"let" : {
				"v" : "$x"
			},
			"as" : "j1",
			"pipeline" : [
				{
					"$match" : {
						"$expr" : {
							"$eq" : [
								"$b",
								"$$v"
							]
						}
					}
				},
				{
					"$project" : {
						"y" : "$b"
					}
				}
			]
		}
	},
	{
		"$unwind" : "$j1"
	}
]
```
### Results
```json
{ "_id" : 1, "j1" : { "_id" : 1, "y" : 1 }, "x" : 1 }
```
### 2-Nodes, Simple rename join preds (subpipeline $match) + Join Optimization
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [y $= x]
leftEmbeddingField: "j1"
rightEmbeddingField: "none"
  |  |
  |  PROJECTION_DEFAULT
  |  transformBy: { "_id" : true, "x" : "$a" }
  |  |
  |  COLLSCAN [test.projections_md_a]
  |  direction: "forward"
  |
  PROJECTION_DEFAULT
  transformBy: { "_id" : true, "y" : "$b" }
  |
  COLLSCAN [test.projections_md_b]
  direction: "forward"
```
## 3. 2-Nodes, Simple rename join preds (trailing $match)
### Pipeline
```json
[
	{
		"$project" : {
			"x" : "$a"
		}
	},
	{
		"$lookup" : {
			"from" : "projections_md_b",
			"as" : "j1",
			"pipeline" : [
				{
					"$project" : {
						"y" : "$b"
					}
				}
			]
		}
	},
	{
		"$unwind" : "$j1"
	},
	{
		"$match" : {
			"$expr" : {
				"$eq" : [
					"$j1.y",
					"$x"
				]
			}
		}
	}
]
```
### Results
```json
{ "_id" : 1, "j1" : { "_id" : 1, "y" : 1 }, "x" : 1 }
```
### 2-Nodes, Simple rename join preds (trailing $match) + Join Optimization
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [y $= x]
leftEmbeddingField: "j1"
rightEmbeddingField: "none"
  |  |
  |  PROJECTION_DEFAULT
  |  transformBy: { "_id" : true, "x" : "$a" }
  |  |
  |  COLLSCAN [test.projections_md_a]
  |  direction: "forward"
  |
  PROJECTION_DEFAULT
  transformBy: { "_id" : true, "y" : "$b" }
  |
  COLLSCAN [test.projections_md_b]
  direction: "forward"
```
## 4. 2-Nodes, Complex rename join preds (trailing $match)
### Pipeline
```json
[
	{
		"$project" : {
			"x" : "$obj.subobj.field"
		}
	},
	{
		"$lookup" : {
			"from" : "projections_md_b",
			"as" : "j1",
			"pipeline" : [
				{
					"$project" : {
						"y" : "$obj.foo"
					}
				}
			]
		}
	},
	{
		"$unwind" : "$j1"
	},
	{
		"$match" : {
			"$expr" : {
				"$eq" : [
					"$j1.y",
					"$x"
				]
			}
		}
	}
]
```
### Results
```json
{ "_id" : 1, "j1" : { "_id" : 1, "y" : "foo" }, "x" : "foo" }
{ "_id" : 2, "j1" : { "_id" : 2, "y" : "bar" }, "x" : "bar" }
{ "_id" : 3, "j1" : { "_id" : 1, "y" : "foo" }, "x" : "foo" }
```
### 2-Nodes, Complex rename join preds (trailing $match) + Join Optimization
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [y $= x]
leftEmbeddingField: "j1"
rightEmbeddingField: "none"
  |  |
  |  PROJECTION_DEFAULT
  |  transformBy: { "_id" : true, "x" : "$obj.subobj.field" }
  |  |
  |  COLLSCAN [test.projections_md_a]
  |  direction: "forward"
  |
  PROJECTION_DEFAULT
  transformBy: { "_id" : true, "y" : "$obj.foo" }
  |
  COLLSCAN [test.projections_md_b]
  direction: "forward"
```
## 5. 4-Nodes, Rename all join preds
### Pipeline
```json
[
	{
		"$project" : {
			"x.y" : "$a",
			"z" : "$obj.subobj.field"
		}
	},
	{
		"$lookup" : {
			"from" : "projections_md_b",
			"as" : "j1",
			"pipeline" : [
				{
					"$project" : {
						"m.n" : "$obj.foo",
						"m.o" : "$b"
					}
				}
			]
		}
	},
	{
		"$unwind" : "$j1"
	},
	{
		"$lookup" : {
			"from" : "projections_md_a",
			"as" : "j2",
			"pipeline" : [ ]
		}
	},
	{
		"$unwind" : "$j2"
	},
	{
		"$lookup" : {
			"from" : "projections_md_b",
			"as" : "j3",
			"pipeline" : [
				{
					"$project" : {
						"obj.obj.obj.foo" : "$obj.foo",
						"obj.obj.b" : "$b"
					}
				}
			]
		}
	},
	{
		"$unwind" : "$j3"
	},
	{
		"$match" : {
			"$expr" : {
				"$and" : [
					{
						"$eq" : [
							"$x.y",
							"$j2.a"
						]
					},
					{
						"$eq" : [
							"$j3.obj.obj.obj.foo",
							"$j2.obj.subobj.field"
						]
					},
					{
						"$eq" : [
							"$z",
							"$j1.m.n"
						]
					},
					{
						"$eq" : [
							"$j1.m.o",
							"$j3.obj.obj.b"
						]
					}
				]
			}
		}
	}
]
```
### Results
```json
{ "_id" : 1, "j1" : { "_id" : 1, "m" : { "n" : "foo", "o" : 1 } }, "j2" : { "_id" : 1, "a" : 1, "obj" : { "subobj" : { "field" : "foo" } } }, "j3" : { "_id" : 1, "obj" : { "obj" : { "b" : 1, "obj" : { "foo" : "foo" } } } }, "x" : { "y" : 1 }, "z" : "foo" }
{ "_id" : 2, "j1" : { "_id" : 2, "m" : { "n" : "bar", "o" : -1 } }, "j2" : { "_id" : 2, "a" : 2, "obj" : { "subobj" : { "field" : "bar" } } }, "j3" : { "_id" : 2, "obj" : { "obj" : { "b" : -1, "obj" : { "foo" : "bar" } } } }, "x" : { "y" : 2 }, "z" : "bar" }
{ "_id" : 3, "j1" : { "_id" : 1, "m" : { "n" : "foo", "o" : 1 } }, "j2" : { "_id" : 3, "a" : 3, "obj" : { "subobj" : { "field" : "foo" } } }, "j3" : { "_id" : 1, "obj" : { "obj" : { "b" : 1, "obj" : { "foo" : "foo" } } } }, "x" : { "y" : 3 }, "z" : "foo" }
```
### 4-Nodes, Rename all join preds + Join Optimization
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [x.y $= a,j3.obj.obj.obj.foo $= obj.subobj.field]
leftEmbeddingField: "none"
rightEmbeddingField: "j2"
  |  |
  |  COLLSCAN [test.projections_md_a]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [j1.m.n $= z]
  leftEmbeddingField: "none"
  rightEmbeddingField: "none"
  |  |
  |  PROJECTION_DEFAULT
  |  transformBy: { "_id" : true, "x" : { "y" : "$a" }, "z" : "$obj.subobj.field" }
  |  |
  |  COLLSCAN [test.projections_md_a]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [m.o $= obj.obj.b]
  leftEmbeddingField: "j1"
  rightEmbeddingField: "j3"
  |  |
  |  PROJECTION_DEFAULT
  |  transformBy: { "_id" : true, "obj" : { "obj" : { "obj" : { "foo" : "$obj.foo" }, "b" : "$b" } } }
  |  |
  |  COLLSCAN [test.projections_md_b]
  |  direction: "forward"
  |
  PROJECTION_DEFAULT
  transformBy: { "_id" : true, "m" : { "n" : "$obj.foo", "o" : "$b" } }
  |
  COLLSCAN [test.projections_md_b]
  direction: "forward"
```
## 6. 2-Nodes, Exclusion projection (local/foreign)
### Pipeline
```json
[
	{
		"$project" : {
			"obj" : 0
		}
	},
	{
		"$lookup" : {
			"from" : "projections_md_b",
			"localField" : "a",
			"foreignField" : "b",
			"as" : "j1",
			"pipeline" : [
				{
					"$project" : {
						"obj" : 0
					}
				}
			]
		}
	},
	{
		"$unwind" : "$j1"
	}
]
```
### Results
```json
{ "_id" : 1, "a" : 1, "j1" : { "_id" : 1, "b" : 1 } }
```
### 2-Nodes, Exclusion projection (local/foreign) + Join Optimization
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [b = a]
leftEmbeddingField: "j1"
rightEmbeddingField: "none"
  |  |
  |  PROJECTION_SIMPLE
  |  transformBy: { "obj" : false }
  |  |
  |  COLLSCAN [test.projections_md_a]
  |  direction: "forward"
  |
  PROJECTION_SIMPLE
  transformBy: { "obj" : false }
  |
  COLLSCAN [test.projections_md_b]
  direction: "forward"
```
## 7. 2-Nodes, Exclusion projection (trailing $match)
### Pipeline
```json
[
	{
		"$project" : {
			"obj" : 0
		}
	},
	{
		"$lookup" : {
			"from" : "projections_md_b",
			"as" : "j1",
			"pipeline" : [
				{
					"$project" : {
						"obj.foo" : 0
					}
				}
			]
		}
	},
	{
		"$unwind" : "$j1"
	},
	{
		"$match" : {
			"$expr" : {
				"$eq" : [
					"$j1.b",
					"$a"
				]
			}
		}
	}
]
```
### Results
```json
{ "_id" : 1, "a" : 1, "j1" : { "_id" : 1, "b" : 1, "obj" : { } } }
```
### 2-Nodes, Exclusion projection (trailing $match) + Join Optimization
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [b $= a]
leftEmbeddingField: "j1"
rightEmbeddingField: "none"
  |  |
  |  PROJECTION_SIMPLE
  |  transformBy: { "obj" : false }
  |  |
  |  COLLSCAN [test.projections_md_a]
  |  direction: "forward"
  |
  PROJECTION_DEFAULT
  transformBy: { "obj" : { "foo" : false } }
  |
  COLLSCAN [test.projections_md_b]
  direction: "forward"
```
## 8. 2-Nodes, Exclusion projection on subobject (trailing $match)
### Pipeline
```json
[
	{
		"$project" : {
			"obj.subobj" : 0
		}
	},
	{
		"$lookup" : {
			"from" : "projections_md_b",
			"as" : "j1",
			"pipeline" : [
				{
					"$project" : {
						"_id" : 0
					}
				}
			]
		}
	},
	{
		"$unwind" : "$j1"
	},
	{
		"$match" : {
			"$expr" : {
				"$eq" : [
					"$j1.b",
					"$a"
				]
			}
		}
	}
]
```
### Results
```json
{ "_id" : 1, "a" : 1, "j1" : { "b" : 1, "obj" : { "foo" : "foo" } }, "obj" : { } }
```
### 2-Nodes, Exclusion projection on subobject (trailing $match) + Join Optimization
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [b $= a]
leftEmbeddingField: "j1"
rightEmbeddingField: "none"
  |  |
  |  PROJECTION_DEFAULT
  |  transformBy: { "obj" : { "subobj" : false } }
  |  |
  |  COLLSCAN [test.projections_md_a]
  |  direction: "forward"
  |
  PROJECTION_SIMPLE
  transformBy: { "_id" : false }
  |
  COLLSCAN [test.projections_md_b]
  direction: "forward"
```
WARNING: results differ from expected!
### Actual results
```json
{ "_id" : 1, "a" : 1, "j1" : { "b" : 1, "obj" : { "foo" : "foo" } }, "obj" : { "subobj" : { "field" : "foo" } } }
```
## 9. 4-Nodes, Rename all join preds, subpipeline edges
### Pipeline
```json
[
	{
		"$project" : {
			"x.y" : "$a",
			"z" : "$obj.subobj.field"
		}
	},
	{
		"$lookup" : {
			"from" : "projections_md_b",
			"localField" : "z",
			"foreignField" : "obj.foo",
			"as" : "j1",
			"pipeline" : [
				{
					"$project" : {
						"m.n" : "$obj.foo",
						"m.o" : "$b"
					}
				}
			]
		}
	},
	{
		"$unwind" : "$j1"
	},
	{
		"$lookup" : {
			"from" : "projections_md_a",
			"as" : "j2",
			"let" : {
				"o1" : "$x.y"
			},
			"pipeline" : [
				{
					"$match" : {
						"$expr" : {
							"$eq" : [
								"$a",
								"$$o1"
							]
						}
					}
				},
				{
					"$project" : {
						"rename.some.field" : "$obj.subobj.field",
						"a" : 1
					}
				}
			]
		}
	},
	{
		"$unwind" : "$j2"
	},
	{
		"$lookup" : {
			"from" : "projections_md_b",
			"as" : "j3",
			"localField" : "j1.m.o",
			"foreignField" : "b",
			"let" : {
				"o2" : "$j2.rename.some.field"
			},
			"pipeline" : [
				{
					"$match" : {
						"$expr" : {
							"$eq" : [
								"$obj.foo",
								"$$o2"
							]
						}
					}
				},
				{
					"$project" : {
						"obj.obj.obj.foo" : "$obj.foo",
						"obj.obj.b" : "$b"
					}
				}
			]
		}
	},
	{
		"$unwind" : "$j3"
	}
]
```
### Results
```json
{ "_id" : 1, "j1" : { "_id" : 1, "m" : { "n" : "foo", "o" : 1 } }, "j2" : { "_id" : 1, "a" : 1, "rename" : { "some" : { "field" : "foo" } } }, "j3" : { "_id" : 1, "obj" : { "obj" : { "b" : 1, "obj" : { "foo" : "foo" } } } }, "x" : { "y" : 1 }, "z" : "foo" }
{ "_id" : 2, "j1" : { "_id" : 2, "m" : { "n" : "bar", "o" : -1 } }, "j2" : { "_id" : 2, "a" : 2, "rename" : { "some" : { "field" : "bar" } } }, "j3" : { "_id" : 2, "obj" : { "obj" : { "b" : -1, "obj" : { "foo" : "bar" } } } }, "x" : { "y" : 2 }, "z" : "bar" }
{ "_id" : 3, "j1" : { "_id" : 1, "m" : { "n" : "foo", "o" : 1 } }, "j2" : { "_id" : 3, "a" : 3, "rename" : { "some" : { "field" : "foo" } } }, "j3" : { "_id" : 1, "obj" : { "obj" : { "b" : 1, "obj" : { "foo" : "foo" } } } }, "x" : { "y" : 3 }, "z" : "foo" }
```
### 4-Nodes, Rename all join preds, subpipeline edges + Join Optimization
usedJoinOptimization: true

```
HASH_JOIN_EMBEDDING [x.y $= a]
leftEmbeddingField: "none"
rightEmbeddingField: "j2"
  |  |
  |  PROJECTION_DEFAULT
  |  transformBy: { "_id" : true, "a" : true, "rename" : { "some" : { "field" : "$obj.subobj.field" } } }
  |  |
  |  COLLSCAN [test.projections_md_a]
  |  direction: "forward"
  |
  HASH_JOIN_EMBEDDING [m.n = z]
  leftEmbeddingField: "j1"
  rightEmbeddingField: "none"
  |  |
  |  PROJECTION_DEFAULT
  |  transformBy: { "_id" : true, "x" : { "y" : "$a" }, "z" : "$obj.subobj.field" }
  |  |
  |  COLLSCAN [test.projections_md_a]
  |  direction: "forward"
  |
  PROJECTION_DEFAULT
  transformBy: { "_id" : true, "m" : { "n" : "$obj.foo", "o" : "$b" } }
  |
  COLLSCAN [test.projections_md_b]
  direction: "forward"
```
