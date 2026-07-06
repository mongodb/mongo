## 1. 3-Node graph, base node fully connected
### No join opt
### Random reordering with seed 0
```
PROJECTION_DEFAULT
transformBy: { "_id" : false, "x" : { "_id" : false }, "y" : { "_id" : false } }
  |
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
### Random reordering with seed 0
```
PROJECTION_DEFAULT
transformBy: { "_id" : false, "x" : { "_id" : false }, "y" : { "_id" : false } }
  |
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
### Random reordering with seed 0
```
PROJECTION_DEFAULT
transformBy: { "_id" : false, "x" : { "_id" : false }, "y" : { "_id" : false } }
  |
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

## 4. 4-Node graph + potentially inferred edges & filters
### No join opt
### Random reordering with seed 0
```
PROJECTION_DEFAULT
transformBy: { "_id" : false, "x" : { "_id" : false }, "y" : { "_id" : false }, "z" : { "_id" : false } }
  |
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

## 5. 5-Node graph + filters
### No join opt
### Random reordering with seed 0
```
PROJECTION_DEFAULT
transformBy: { "_id" : false, "aaa" : { "_id" : false }, "bbb" : { "_id" : false }, "ccc" : { "_id" : false }, "ddd" : { "_id" : false } }
  |
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

