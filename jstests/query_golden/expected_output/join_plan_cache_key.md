## Queries where identical join plan cache keys are expected
### Identical keys for completely identical queries
Command 1: {"aggregate":"foo","pipeline":[{"$lookup":{"from":"bar","localField":"a","foreignField":"a","as":"bar"}},{"$unwind":"$bar"}]}  
Command 2: {"aggregate":"foo","pipeline":[{"$lookup":{"from":"bar","localField":"a","foreignField":"a","as":"bar"}},{"$unwind":"$bar"}]}  
Hash 1: C161941F  
Hash 2: C161941F  
### Identical keys for queries with different suffixes
Command 1: {"aggregate":"foo","pipeline":[{"$lookup":{"from":"bar","localField":"a","foreignField":"a","as":"bar"}},{"$unwind":"$bar"},{"$sort":{"a":1}}]}  
Command 2: {"aggregate":"foo","pipeline":[{"$lookup":{"from":"bar","localField":"a","foreignField":"a","as":"bar"}},{"$unwind":"$bar"},{"$sort":{"b":1}}]}  
Hash 1: C161941F  
Hash 2: C161941F  
### Identical keys for queries with different literals in prefix $match
Command 1: {"aggregate":"foo","pipeline":[{"$match":{"a":1}},{"$lookup":{"from":"bar","localField":"a","foreignField":"a","as":"bar"}},{"$unwind":"$bar"}]}  
Command 2: {"aggregate":"foo","pipeline":[{"$match":{"a":2}},{"$lookup":{"from":"bar","localField":"a","foreignField":"a","as":"bar"}},{"$unwind":"$bar"}]}  
Hash 1: DD95352F  
Hash 2: DD95352F  
### Identical keys for queries with different literals in suffix $match
Command 1: {"aggregate":"foo","pipeline":[{"$lookup":{"from":"bar","localField":"a","foreignField":"a","as":"bar"}},{"$unwind":"$bar"},{"$match":{"a":1}}]}  
Command 2: {"aggregate":"foo","pipeline":[{"$lookup":{"from":"bar","localField":"a","foreignField":"a","as":"bar"}},{"$unwind":"$bar"},{"$match":{"a":2}}]}  
Hash 1: DD95352F  
Hash 2: DD95352F  
### Identical keys for queries with different literals in subpipeline $match
Command 1: {"aggregate":"foo","pipeline":[{"$lookup":{"from":"bar","localField":"a","foreignField":"a","as":"bar","pipeline":[{"$match":{"a":1}}]}},{"$unwind":"$bar"}]}  
Command 2: {"aggregate":"foo","pipeline":[{"$lookup":{"from":"bar","localField":"a","foreignField":"a","as":"bar","pipeline":[{"$match":{"a":2}}]}},{"$unwind":"$bar"}]}  
Hash 1: DD95352F  
Hash 2: DD95352F  
### Identical keys for queries that are semantically equivalent after predicate pushdown
Command 1: {"aggregate":"foo","pipeline":[{"$lookup":{"from":"bar","localField":"a","foreignField":"a","as":"bar","pipeline":[{"$match":{"a":1}}]}},{"$unwind":"$bar"}]}  
Command 2: {"aggregate":"foo","pipeline":[{"$match":{"a":1}},{"$lookup":{"from":"bar","localField":"a","foreignField":"a","as":"bar"}},{"$unwind":"$bar"}]}  
Hash 1: DD95352F  
Hash 2: DD95352F  
### Identical keys for queries with different $in lists
Command 1: {"aggregate":"foo","pipeline":[{"$match":{"a":{"$in":[1,2]}}},{"$lookup":{"from":"bar","localField":"a","foreignField":"a","as":"bar"}},{"$unwind":"$bar"}]}  
Command 2: {"aggregate":"foo","pipeline":[{"$match":{"a":{"$in":[2,3]}}},{"$lookup":{"from":"bar","localField":"a","foreignField":"a","as":"bar"}},{"$unwind":"$bar"}]}  
Hash 1: 61042BC2  
Hash 2: 61042BC2  
### Identical keys for queries with different $in list lengths
Command 1: {"aggregate":"foo","pipeline":[{"$match":{"a":{"$in":[1,2]}}},{"$lookup":{"from":"bar","localField":"a","foreignField":"a","as":"bar"}},{"$unwind":"$bar"}]}  
Command 2: {"aggregate":"foo","pipeline":[{"$match":{"a":{"$in":[1,2,3]}}},{"$lookup":{"from":"bar","localField":"a","foreignField":"a","as":"bar"}},{"$unwind":"$bar"}]}  
Hash 1: 61042BC2  
Hash 2: 61042BC2  
### Identical keys for queries with $expr in subpipeline with no outer references
Command 1: {"aggregate":"foo","pipeline":[{"$lookup":{"from":"bar","localField":"a","foreignField":"a","as":"bar","pipeline":[{"$match":{"$expr":{"$eq":["$a",1]}}}]}},{"$unwind":"$bar"}]}  
Command 2: {"aggregate":"foo","pipeline":[{"$lookup":{"from":"bar","localField":"a","foreignField":"a","as":"bar","pipeline":[{"$match":{"$expr":{"$eq":["$a",2]}}}]}},{"$unwind":"$bar"}]}  
Hash 1: FAC4B54C  
Hash 2: FAC4B54C  
### Identical keys for queries with let argument to aggregate() used in $expr
Command 1: {"aggregate":"foo","pipeline":[{"$match":{"$expr":{"$eq":["$a","$$var"]}}},{"$lookup":{"from":"bar","localField":"a","foreignField":"a","as":"bar"}},{"$unwind":"$bar"}],"let":{"var":1}}  
Command 2: {"aggregate":"foo","pipeline":[{"$match":{"$expr":{"$eq":["$a","$$var"]}}},{"$lookup":{"from":"bar","localField":"a","foreignField":"a","as":"bar"}},{"$unwind":"$bar"}],"let":{"var":2}}  
Hash 1: 91DE93BF  
Hash 2: 91DE93BF  
### Identical keys for queries with variables in the let argument to aggregate() that are named differently
Command 1: {"aggregate":"foo","pipeline":[{"$match":{"$expr":{"$eq":["$a","$$var1"]}}},{"$lookup":{"from":"bar","localField":"a","foreignField":"a","as":"bar"}},{"$unwind":"$bar"}],"let":{"var1":1}}  
Command 2: {"aggregate":"foo","pipeline":[{"$match":{"$expr":{"$eq":["$a","$$var2"]}}},{"$lookup":{"from":"bar","localField":"a","foreignField":"a","as":"bar"}},{"$unwind":"$bar"}],"let":{"var2":2}}  
Hash 1: 91DE93BF  
Hash 2: 91DE93BF  
## Queries where different join plan cache keys are expected
### Different keys for different base collections
Command 1: {"aggregate":"foo","pipeline":[{"$lookup":{"from":"bar","localField":"a","foreignField":"a","as":"bar"}},{"$unwind":"$bar"}]}  
Command 2: {"aggregate":"foo2","pipeline":[{"$lookup":{"from":"bar","localField":"a","foreignField":"a","as":"bar"}},{"$unwind":"$bar"}]}  
Hash 1: C161941F  
Hash 2: 40843C41  
### Different keys for different join collections
Command 1: {"aggregate":"foo","pipeline":[{"$lookup":{"from":"bar","localField":"a","foreignField":"a","as":"bar"}},{"$unwind":"$bar"}]}  
Command 2: {"aggregate":"foo","pipeline":[{"$lookup":{"from":"bar2","localField":"a","foreignField":"a","as":"bar2"}},{"$unwind":"$bar2"}]}  
Hash 1: C161941F  
Hash 2: 39F550E3  
### Different keys for different predicates in prefix $match
Command 1: {"aggregate":"foo","pipeline":[{"$match":{"a":1}},{"$lookup":{"from":"bar","localField":"a","foreignField":"a","as":"bar"}},{"$unwind":"$bar"}]}  
Command 2: {"aggregate":"foo","pipeline":[{"$match":{"b":1}},{"$lookup":{"from":"bar","localField":"a","foreignField":"a","as":"bar"}},{"$unwind":"$bar"}]}  
Hash 1: DD95352F  
Hash 2: 3F5F7E27  
### Different keys for different predicates in suffix $match
Command 1: {"aggregate":"foo","pipeline":[{"$lookup":{"from":"bar","localField":"a","foreignField":"a","as":"bar"}},{"$unwind":"$bar"},{"$match":{"a":1}}]}  
Command 2: {"aggregate":"foo","pipeline":[{"$lookup":{"from":"bar","localField":"a","foreignField":"a","as":"bar"}},{"$unwind":"$bar"},{"$match":{"b":1}}]}  
Hash 1: DD95352F  
Hash 2: 3F5F7E27  
### Different keys for different predicates in subpipeline $match
Command 1: {"aggregate":"foo","pipeline":[{"$lookup":{"from":"bar","localField":"a","foreignField":"a","as":"bar","pipeline":[{"$match":{"a":1}}]}},{"$unwind":"$bar"}]}  
Command 2: {"aggregate":"foo","pipeline":[{"$lookup":{"from":"bar","localField":"a","foreignField":"a","as":"bar","pipeline":[{"$match":{"b":1}}]}},{"$unwind":"$bar"}]}  
Hash 1: DD95352F  
Hash 2: B544BC9A  
### Different keys for different localField
Command 1: {"aggregate":"foo","pipeline":[{"$lookup":{"from":"bar","localField":"a","foreignField":"a","as":"bar"}},{"$unwind":"$bar"}]}  
Command 2: {"aggregate":"foo","pipeline":[{"$lookup":{"from":"bar","localField":"b","foreignField":"a","as":"bar"}},{"$unwind":"$bar"}]}  
Hash 1: C161941F  
Hash 2: E6D387DD  
### Different keys for different foreignField
Command 1: {"aggregate":"foo","pipeline":[{"$lookup":{"from":"bar","localField":"a","foreignField":"a","as":"bar"}},{"$unwind":"$bar"}]}  
Command 2: {"aggregate":"foo","pipeline":[{"$lookup":{"from":"bar","localField":"a","foreignField":"b","as":"bar"}},{"$unwind":"$bar"}]}  
Hash 1: C161941F  
Hash 2: B645791A  
### Different keys for different as in $lookup
Command 1: {"aggregate":"foo","pipeline":[{"$lookup":{"from":"bar","localField":"a","foreignField":"a","as":"bar1"}},{"$unwind":"$bar1"}]}  
Command 2: {"aggregate":"foo","pipeline":[{"$lookup":{"from":"bar","localField":"a","foreignField":"a","as":"bar2"}},{"$unwind":"$bar2"}]}  
Hash 1: 03AE615C  
Hash 2: A36B6538  
### Different keys for queries that are not semantically equivalent after predicate pushdown
Command 1: {"aggregate":"foo","pipeline":[{"$lookup":{"from":"bar","localField":"a","foreignField":"a","as":"bar","pipeline":[{"$match":{"a":1}}]}},{"$unwind":"$bar"}]}  
Command 2: {"aggregate":"foo","pipeline":[{"$match":{"a":{"$gt":1}}},{"$lookup":{"from":"bar","localField":"a","foreignField":"a","as":"bar"}},{"$unwind":"$bar"}]}  
Hash 1: DD95352F  
Hash 2: FF5AC6A6  
### Different keys for queries with different $project plan suffixes
Command 1: {"aggregate":"foo","pipeline":[{"$lookup":{"from":"bar","localField":"a","foreignField":"a","as":"bar"}},{"$unwind":"$bar"},{"$project":{"a":1}}]}  
Command 2: {"aggregate":"foo","pipeline":[{"$lookup":{"from":"bar","localField":"a","foreignField":"a","as":"bar"}},{"$unwind":"$bar"},{"$project":{"b":1}}]}  
Hash 1: 5316786E  
Hash 2: D55A8C7D  
### TODO(SERVER-121078): Different keys with $limit and no $limit
Command 1: {"aggregate":"foo","pipeline":[{"$lookup":{"from":"bar","localField":"a","foreignField":"a","as":"bar"}},{"$unwind":"$bar"}]}  
Command 2: {"aggregate":"foo","pipeline":[{"$lookup":{"from":"bar","localField":"a","foreignField":"a","as":"bar"}},{"$unwind":"$bar"},{"$limit":1}]}  
Hash 1: C161941F  
Hash 2: C161941F  
> [!WARNING]
> Hash keys were expected to be different but were not!
### TODO(SERVER-131472): Different keys with allowDiskUse: true and false
Command 1: {"aggregate":"foo","pipeline":[{"$lookup":{"from":"bar","localField":"a","foreignField":"a","as":"bar"}},{"$unwind":"$bar"}],"allowDiskUse":true}  
Command 2: {"aggregate":"foo","pipeline":[{"$lookup":{"from":"bar","localField":"a","foreignField":"a","as":"bar"}},{"$unwind":"$bar"}],"allowDiskUse":false}  
Hash 1: C161941F  
Hash 2: C161941F  
> [!WARNING]
> Hash keys were expected to be different but were not!
### Different keys for queries with different fields in $expr
Command 1: {"aggregate":"foo","pipeline":[{"$match":{"$expr":{"$eq":["$a","$$var"]}}},{"$lookup":{"from":"bar","localField":"a","foreignField":"a","as":"bar"}},{"$unwind":"$bar"}],"let":{"var":1}}  
Command 2: {"aggregate":"foo","pipeline":[{"$match":{"$expr":{"$eq":["$b","$$var"]}}},{"$lookup":{"from":"bar","localField":"a","foreignField":"a","as":"bar"}},{"$unwind":"$bar"}],"let":{"var":1}}  
Hash 1: 91DE93BF  
Hash 2: 11614ABD  
### Different keys for queries with $unwind with preserveNullAndEmptyArrays true/false
Command 1: {"aggregate":"foo","pipeline":[{"$lookup":{"from":"bar","localField":"a","foreignField":"a","as":"bar"}},{"$unwind":{"path":"$bar","preserveNullAndEmptyArrays":true}}]}  
Command 2: {"aggregate":"foo","pipeline":[{"$lookup":{"from":"bar","localField":"a","foreignField":"a","as":"bar"}},{"$unwind":{"path":"$bar","preserveNullAndEmptyArrays":false}}]}  
Hash 1: undefined  
Hash 2: C161941F  
### TODO(SERVER-131752) Different keys expected for queries with and without $_internalJoinHint
Command 1: {"aggregate":"foo","pipeline":[{"$lookup":{"from":"bar","localField":"a","foreignField":"a","as":"bar"}},{"$unwind":"$bar"}]}  
Command 2: {"aggregate":"foo","pipeline":[{"$_internalJoinHint":{"perSubsetLevelMode":[{"level":0,"mode":"ALL"}]}},{"$lookup":{"from":"bar","localField":"a","foreignField":"a","as":"bar"}},{"$unwind":"$bar"}]}  
Hash 1: C161941F  
Hash 2: C161941F  
> [!WARNING]
> Hash keys were expected to be different but were not!
### TODO(SERVER-131752) Different keys expected for queries with different $_internalJoinHint
Command 1: {"aggregate":"foo","pipeline":[{"$_internalJoinHint":{"perSubsetLevelMode":[{"level":0,"hint":{"method":"HJ"},"mode":"ALL"}]}},{"$lookup":{"from":"bar","localField":"a","foreignField":"a","as":"bar"}},{"$unwind":"$bar"}]}  
Command 2: {"aggregate":"foo","pipeline":[{"$_internalJoinHint":{"perSubsetLevelMode":[{"level":0,"hint":{"method":"INLJ"},"mode":"ALL"}]}},{"$lookup":{"from":"bar","localField":"a","foreignField":"a","as":"bar"}},{"$unwind":"$bar"}]}  
Hash 1: C161941F  
Hash 2: C161941F  
> [!WARNING]
> Hash keys were expected to be different but were not!
## Queries that currently do not have a join plan cache key
### TODO(SERVER-115652): Currently no hashes for queries with let argument to $lookup
Command 1: {"aggregate":"foo","pipeline":[{"$lookup":{"from":"bar","localField":"a","foreignField":"a","as":"bar","pipeline":[{"$match":{"$expr":{"$eq":["$a","$$var"]}}}],"let":{"var":2}}},{"$unwind":"$bar"}]}  
Command 2: {"aggregate":"foo","pipeline":[{"$lookup":{"from":"bar","localField":"a","foreignField":"a","as":"bar","pipeline":[{"$match":{"$expr":{"$eq":["$a","$$var"]}}}],"let":{"var":3}}},{"$unwind":"$bar"}]}  
Hash 1: undefined  
Hash 2: undefined  
### Currently no hashes for queries with aggregate() with collation
Command 1: {"aggregate":"foo","pipeline":[{"$lookup":{"from":"bar","localField":"a","foreignField":"a","as":"bar"}},{"$unwind":"$bar"}],"collation":{"locale":"en_US","strength":1}}  
Command 2: {"aggregate":"foo","pipeline":[{"$lookup":{"from":"bar","localField":"a","foreignField":"a","as":"bar"}},{"$unwind":"$bar"}],"collation":{"locale":"en_US","strength":2}}  
Hash 1: undefined  
Hash 2: undefined  
### Currently no hashes for queries with $unwind with includeArrayIndex
Command 1: {"aggregate":"foo","pipeline":[{"$lookup":{"from":"bar","localField":"a","foreignField":"a","as":"bar"}},{"$unwind":{"path":"$bar","includeArrayIndex":"arrayIndex"}}]}  
Command 2: {"aggregate":"foo","pipeline":[{"$lookup":{"from":"bar","localField":"a","foreignField":"a","as":"bar"}},{"$unwind":{"path":"$bar","includeArrayIndex":"arrayIndex2"}}]}  
Hash 1: undefined  
Hash 2: undefined  
### Currently no hashes for queries with aggregate()-level hints
Command 1: {"aggregate":"foo","pipeline":[{"$lookup":{"from":"bar","localField":"a","foreignField":"a","as":"bar"}},{"$unwind":"$bar"}],"hint":{"a":1}}  
Command 2: {"aggregate":"foo","pipeline":[{"$lookup":{"from":"bar","localField":"a","foreignField":"a","as":"bar"}},{"$unwind":"$bar"}],"hint":{"b":1}}  
Hash 1: undefined  
Hash 2: undefined  
