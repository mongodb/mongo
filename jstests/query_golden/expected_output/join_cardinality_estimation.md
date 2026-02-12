# One-to-one joins
```js
db.many_rows.aggregate([{"$lookup":{"from":"many_rows","localField":"i_idx","foreignField":"i_idx","as":"right"}},{"$unwind":"$right"},{"$match":{}}])
```
Actual cardinality: 1000  
Estimated cardinality: 1000  
Close enough?: yes

```js
db.many_rows.aggregate([{"$lookup":{"from":"many_rows","as":"right","let":{"localField":"$i_idx"},"pipeline":[{"$match":{"$and":[{"$expr":{"$eq":["$$localField","$i_idx"]}},{}]}}]}},{"$unwind":"$right"},{"$match":{}}])
```
Actual cardinality: 1000  
Estimated cardinality: 1000  
Close enough?: yes

# One-to-many joins
```js
db.many_rows.aggregate([{"$lookup":{"from":"many_rows","localField":"i_idx","foreignField":"d_idx","as":"right"}},{"$unwind":"$right"},{"$match":{}}])
```
Actual cardinality: 1000  
Estimated cardinality: 1000  
Close enough?: yes

```js
db.many_rows.aggregate([{"$lookup":{"from":"many_rows","localField":"i_idx","foreignField":"c_idx","as":"right"}},{"$unwind":"$right"},{"$match":{}}])
```
Actual cardinality: 1000  
Estimated cardinality: 1000  
Close enough?: yes

```js
db.many_rows.aggregate([{"$lookup":{"from":"many_rows","as":"right","let":{"localField":"$i_idx"},"pipeline":[{"$match":{"$and":[{"$expr":{"$eq":["$$localField","$d_idx"]}},{}]}}]}},{"$unwind":"$right"},{"$match":{}}])
```
Actual cardinality: 1000  
Estimated cardinality: 1000  
Close enough?: yes

```js
db.many_rows.aggregate([{"$lookup":{"from":"many_rows","as":"right","let":{"localField":"$i_idx"},"pipeline":[{"$match":{"$and":[{"$expr":{"$eq":["$$localField","$c_idx"]}},{}]}}]}},{"$unwind":"$right"},{"$match":{}}])
```
Actual cardinality: 1000  
Estimated cardinality: 1000  
Close enough?: yes

# Many-to-one joins
```js
db.many_rows.aggregate([{"$lookup":{"from":"many_rows","localField":"c_idx","foreignField":"i_idx","as":"right"}},{"$unwind":"$right"},{"$match":{}}])
```
Actual cardinality: 1000  
Estimated cardinality: 1000000  
Close enough?: NO

```js
db.many_rows.aggregate([{"$lookup":{"from":"many_rows","as":"right","let":{"localField":"$c_idx"},"pipeline":[{"$match":{"$and":[{"$expr":{"$eq":["$$localField","$i_idx"]}},{}]}}]}},{"$unwind":"$right"},{"$match":{}}])
```
Actual cardinality: 1000  
Estimated cardinality: 1000000  
Close enough?: NO

# Many-to-many joins
```js
db.many_rows.aggregate([{"$lookup":{"from":"many_rows","localField":"d_idx","foreignField":"d_idx","as":"right"}},{"$unwind":"$right"},{"$match":{}}])
```
Actual cardinality: 100000  
Estimated cardinality: 100000  
Close enough?: yes

```js
db.many_rows.aggregate([{"$lookup":{"from":"many_rows","as":"right","let":{"localField":"$d_idx"},"pipeline":[{"$match":{"$and":[{"$expr":{"$eq":["$$localField","$d_idx"]}},{}]}}]}},{"$unwind":"$right"},{"$match":{}}])
```
Actual cardinality: 100000  
Estimated cardinality: 100000  
Close enough?: yes

# Cross joins
```js
db.many_rows.aggregate([{"$lookup":{"from":"many_rows","localField":"c_idx","foreignField":"c_idx","as":"right"}},{"$unwind":"$right"},{"$match":{}}])
```
Actual cardinality: 1000000  
Estimated cardinality: 1000000  
Close enough?: yes

```js
db.many_rows.aggregate([{"$lookup":{"from":"many_rows","as":"right","let":{"localField":"$c_idx"},"pipeline":[{"$match":{"$and":[{"$expr":{"$eq":["$$localField","$c_idx"]}},{}]}}]}},{"$unwind":"$right"},{"$match":{}}])
```
Actual cardinality: 1000000  
Estimated cardinality: 1000000  
Close enough?: yes

# Joins on a unique index
```js
db.many_rows.aggregate([{"$lookup":{"from":"many_rows","localField":"i_idx_unique","foreignField":"d_idx","as":"right"}},{"$unwind":"$right"},{"$match":{}}])
```
Actual cardinality: 1000  
Estimated cardinality: 1000  
Close enough?: yes

```js
db.many_rows.aggregate([{"$lookup":{"from":"many_rows","localField":"d_idx","foreignField":"i_idx_unique","as":"right"}},{"$unwind":"$right"},{"$match":{}}])
```
Actual cardinality: 1000  
Estimated cardinality: 100000  
Close enough?: NO

# Join where there is an index only on one side
```js
db.many_rows.aggregate([{"$lookup":{"from":"many_rows","localField":"i_idx","foreignField":"i_noidx","as":"right"}},{"$unwind":"$right"},{"$match":{}}])
```
Actual cardinality: 1000  
Estimated cardinality: 1000  
Close enough?: yes

```js
db.many_rows.aggregate([{"$lookup":{"from":"many_rows","localField":"i_noidx","foreignField":"i_idx","as":"right"}},{"$unwind":"$right"},{"$match":{}}])
```
Actual cardinality: 1000  
Estimated cardinality: 1000  
Close enough?: yes

# Join with no indexes on either side
```js
db.many_rows.aggregate([{"$lookup":{"from":"many_rows","localField":"i_noidx","foreignField":"i_noidx","as":"right"}},{"$unwind":"$right"},{"$match":{}}])
```
Actual cardinality: 1000  
Estimated cardinality: 1000  
Close enough?: yes

# Join where no values match the join predicate
```js
db.many_rows.aggregate([{"$lookup":{"from":"many_rows","localField":"c_idx","foreignField":"i_idx_offset","as":"right"}},{"$unwind":"$right"},{"$match":{}}])
```
Actual cardinality: 0  
Estimated cardinality: 1000000  
Close enough?: NO

# Joins on an empty collection
```js
db.no_rows.aggregate([{"$lookup":{"from":"many_rows","localField":"i_idx","foreignField":"i_idx","as":"right"}},{"$unwind":"$right"},{"$match":{}}])
```
Actual cardinality: 0  
Estimated cardinality: 0  
Close enough?: yes

```js
db.many_rows.aggregate([{"$lookup":{"from":"no_rows","localField":"i_idx","foreignField":"i_idx","as":"right"}},{"$unwind":"$right"},{"$match":{}}])
```
Actual cardinality: 0  
Estimated cardinality: 0  
Close enough?: yes

```js
db.no_rows.aggregate([{"$lookup":{"from":"no_rows","localField":"i_idx","foreignField":"i_idx","as":"right"}},{"$unwind":"$right"},{"$match":{}}])
```
Actual cardinality: 0  
Estimated cardinality: 0  
Close enough?: yes

# Joins on a one-document input
```js
db.one_row.aggregate([{"$lookup":{"from":"many_rows","localField":"i_idx","foreignField":"i_idx","as":"right"}},{"$unwind":"$right"},{"$match":{}}])
```
Actual cardinality: 1  
Estimated cardinality: 1000  
Close enough?: NO

```js
db.many_rows.aggregate([{"$lookup":{"from":"one_row","localField":"i_idx","foreignField":"i_idx","as":"right"}},{"$unwind":"$right"},{"$match":{}}])
```
Actual cardinality: 1  
Estimated cardinality: 1000  
Close enough?: NO

```js
db.many_rows.aggregate([{"$lookup":{"from":"one_row","localField":"i_idx","foreignField":"i_idx","as":"right"}},{"$unwind":"$right"},{"$match":{"i_idx":1}}])
```
Actual cardinality: 1  
Estimated cardinality: 1  
Close enough?: yes

```js
db.many_rows.aggregate([{"$lookup":{"from":"many_rows","localField":"i_idx","foreignField":"i_idx","as":"right"}},{"$unwind":"$right"},{"$match":{"i_idx":1}}])
```
Actual cardinality: 1  
Estimated cardinality: 1  
Close enough?: yes

```js
db.many_rows.aggregate([{"$lookup":{"from":"many_rows","as":"right","let":{"localField":"$i_idx"},"pipeline":[{"$match":{"$and":[{"$expr":{"$eq":["$$localField","$i_idx"]}},{"i_idx":1}]}}]}},{"$unwind":"$right"},{"$match":{}}])
```
Actual cardinality: 1  
Estimated cardinality: 1  
Close enough?: yes

```js
db.one_row.aggregate([{"$lookup":{"from":"many_rows","as":"right","let":{"localField":"$i_idx"},"pipeline":[{"$match":{"$and":[{"$expr":{"$eq":["$$localField","$i_idx"]}},{"i_idx":1}]}}]}},{"$unwind":"$right"},{"$match":{}}])
```
Actual cardinality: 1  
Estimated cardinality: 1  
Close enough?: yes

# Join on a missing field
```js
db.many_rows.aggregate([{"$lookup":{"from":"many_rows","localField":"missing_field","foreignField":"i_idx","as":"right"}},{"$unwind":"$right"},{"$match":{}}])
```
Actual cardinality: 0  
Estimated cardinality: 1000000  
Close enough?: NO

```js
db.many_rows.aggregate([{"$lookup":{"from":"many_rows","localField":"i_idx","foreignField":"missing_field","as":"right"}},{"$unwind":"$right"},{"$match":{}}])
```
Actual cardinality: 0  
Estimated cardinality: 1000  
Close enough?: NO

```js
db.many_rows.aggregate([{"$lookup":{"from":"many_rows","localField":"missing_field","foreignField":"missing_field","as":"right"}},{"$unwind":"$right"},{"$match":{}}])
```
Actual cardinality: 1000000  
Estimated cardinality: 1000000  
Close enough?: yes

# Join on a field with all nulls
```js
db.many_rows.aggregate([{"$lookup":{"from":"many_rows","localField":"n_idx","foreignField":"i_idx","as":"right"}},{"$unwind":"$right"},{"$match":{}}])
```
Actual cardinality: 0  
Estimated cardinality: 1000000  
Close enough?: NO

```js
db.many_rows.aggregate([{"$lookup":{"from":"many_rows","localField":"i_idx","foreignField":"n_idx","as":"right"}},{"$unwind":"$right"},{"$match":{}}])
```
Actual cardinality: 0  
Estimated cardinality: 1000  
Close enough?: NO

```js
db.many_rows.aggregate([{"$lookup":{"from":"many_rows","localField":"n_idx","foreignField":"n_idx","as":"right"}},{"$unwind":"$right"},{"$match":{}}])
```
Actual cardinality: 1000000  
Estimated cardinality: 1000000  
Close enough?: yes

# Join on a field with mosly nulls
```js
db.many_rows.aggregate([{"$lookup":{"from":"mostly_nulls","localField":"i_idx","foreignField":"i_idx","as":"right"}},{"$unwind":"$right"},{"$match":{"i_idx":{"$ne":null}}}])
```
Actual cardinality: 1  
Estimated cardinality: 1001  
Close enough?: NO

```js
db.mostly_nulls.aggregate([{"$lookup":{"from":"many_rows","localField":"i_idx","foreignField":"i_idx","as":"right"}},{"$unwind":"$right"},{"$match":{"i_idx":{"$ne":null}}}])
```
Actual cardinality: 1  
Estimated cardinality: 1.001  
Close enough?: yes

```js
db.mostly_nulls.aggregate([{"$lookup":{"from":"mostly_nulls","localField":"i_idx","foreignField":"i_idx","as":"right"}},{"$unwind":"$right"},{"$match":{"i_idx":{"$ne":null}}}])
```
Actual cardinality: 1  
Estimated cardinality: 501.00049999999993  
Close enough?: NO

# Joins with filter on the left side over the join field
```js
db.many_rows.aggregate([{"$lookup":{"from":"many_rows","localField":"i_idx","foreignField":"i_idx","as":"right"}},{"$unwind":"$right"},{"$match":{"i_idx":1}}])
```
Actual cardinality: 1  
Estimated cardinality: 1  
Close enough?: yes

```js
db.many_rows.aggregate([{"$lookup":{"from":"many_rows","localField":"d_idx","foreignField":"i_idx","as":"right"}},{"$unwind":"$right"},{"$match":{"d_idx":1}}])
```
Actual cardinality: 100  
Estimated cardinality: 10000  
Close enough?: NO

```js
db.many_rows.aggregate([{"$lookup":{"from":"many_rows","localField":"i_idx","foreignField":"i_idx","as":"right"}},{"$unwind":"$right"},{"$match":{"i_idx":{"$lt":50}}}])
```
Actual cardinality: 50  
Estimated cardinality: 50  
Close enough?: yes

# Joins with filter on the left side over another field (residual predicate)
```js
db.many_rows.aggregate([{"$lookup":{"from":"many_rows","localField":"i_idx","foreignField":"i_idx","as":"right"}},{"$unwind":"$right"},{"$match":{"d_idx":1}}])
```
Actual cardinality: 100  
Estimated cardinality: 100  
Close enough?: yes

```js
db.many_rows.aggregate([{"$lookup":{"from":"many_rows","localField":"i_idx","foreignField":"i_idx","as":"right"}},{"$unwind":"$right"},{"$match":{"d_idx":{"$lt":5}}}])
```
Actual cardinality: 500  
Estimated cardinality: 500  
Close enough?: yes

# Joins with unsatisfiable filter on left side
```js
db.many_rows.aggregate([{"$lookup":{"from":"many_rows","localField":"i_idx","foreignField":"i_idx","as":"right"}},{"$unwind":"$right"},{"$match":{"i_idx":{"$lt":0}}}])
```
Actual cardinality: 0  
Estimated cardinality: 0  
Close enough?: yes

```js
db.many_rows.aggregate([{"$lookup":{"from":"many_rows","localField":"c_idx","foreignField":"i_idx","as":"right"}},{"$unwind":"$right"},{"$match":{"c_idx":0}}])
```
Actual cardinality: 0  
Estimated cardinality: 0  
Close enough?: yes

# Join with filters on the right side over the join field
```js
db.many_rows.aggregate([{"$lookup":{"from":"many_rows","as":"right","let":{"localField":"$i_idx"},"pipeline":[{"$match":{"$and":[{"$expr":{"$eq":["$$localField","$i_idx"]}},{"i_idx":1}]}}]}},{"$unwind":"$right"},{"$match":{}}])
```
Actual cardinality: 1  
Estimated cardinality: 1  
Close enough?: yes

```js
db.many_rows.aggregate([{"$lookup":{"from":"many_rows","as":"right","let":{"localField":"$i_idx"},"pipeline":[{"$match":{"$and":[{"$expr":{"$eq":["$$localField","$i_idx"]}},{"i_idx":{"$ne":1}}]}}]}},{"$unwind":"$right"},{"$match":{}}])
```
Actual cardinality: 999  
Estimated cardinality: 999  
Close enough?: yes

```js
db.many_rows.aggregate([{"$lookup":{"from":"many_rows","as":"right","let":{"localField":"$d_idx"},"pipeline":[{"$match":{"$and":[{"$expr":{"$eq":["$$localField","$i_idx"]}},{"i_idx":1}]}}]}},{"$unwind":"$right"},{"$match":{}}])
```
Actual cardinality: 100  
Estimated cardinality: 100  
Close enough?: yes

# Join with filters on the right side over another field (residual predicate)
```js
db.many_rows.aggregate([{"$lookup":{"from":"many_rows","as":"right","let":{"localField":"$i_idx"},"pipeline":[{"$match":{"$and":[{"$expr":{"$eq":["$$localField","$i_idx"]}},{"d_idx":1}]}}]}},{"$unwind":"$right"},{"$match":{}}])
```
Actual cardinality: 100  
Estimated cardinality: 100  
Close enough?: yes

```js
db.many_rows.aggregate([{"$lookup":{"from":"many_rows","as":"right","let":{"localField":"$d_idx"},"pipeline":[{"$match":{"$and":[{"$expr":{"$eq":["$$localField","$d_idx"]}},{"i_idx":1}]}}]}},{"$unwind":"$right"},{"$match":{}}])
```
Actual cardinality: 100  
Estimated cardinality: 100  
Close enough?: yes

# Right side filter matches all rows
```js
db.many_rows.aggregate([{"$lookup":{"from":"many_rows","as":"right","let":{"localField":"$i_idx"},"pipeline":[{"$match":{"$and":[{"$expr":{"$eq":["$$localField","$i_idx"]}},{"i_idx":{"$gte":0}}]}}]}},{"$unwind":"$right"},{"$match":{}}])
```
Actual cardinality: 1000  
Estimated cardinality: 1000  
Close enough?: yes

```js
db.many_rows.aggregate([{"$lookup":{"from":"many_rows","as":"right","let":{"localField":"$i_idx"},"pipeline":[{"$match":{"$and":[{"$expr":{"$eq":["$$localField","$i_idx"]}},{"c_idx":1}]}}]}},{"$unwind":"$right"},{"$match":{}}])
```
Actual cardinality: 1000  
Estimated cardinality: 1000  
Close enough?: yes

# Right side filter matches no rows
```js
db.many_rows.aggregate([{"$lookup":{"from":"many_rows","as":"right","let":{"localField":"$i_idx"},"pipeline":[{"$match":{"$and":[{"$expr":{"$eq":["$$localField","$i_idx"]}},{"i_idx":-999}]}}]}},{"$unwind":"$right"},{"$match":{}}])
```
Actual cardinality: 0  
Estimated cardinality: 0  
Close enough?: yes

```js
db.many_rows.aggregate([{"$lookup":{"from":"many_rows","as":"right","let":{"localField":"$i_idx"},"pipeline":[{"$match":{"$and":[{"$expr":{"$eq":["$$localField","$i_idx"]}},{"d_idx":11}]}}]}},{"$unwind":"$right"},{"$match":{}}])
```
Actual cardinality: 0  
Estimated cardinality: 0  
Close enough?: yes

# Multi-table joins - chain
## Same join key throughout
```js
db.many_rows.aggregate([{"$lookup":{"from":"many_rows","localField":"i_idx","foreignField":"i_idx","as":"right1"}},{"$unwind":"$right1"},{"$lookup":{"from":"many_rows","localField":"i_idx","foreignField":"i_idx","as":"right2"}},{"$unwind":"$right2"},{"$match":{}}])
```
Actual cardinality: 1000  
Estimated cardinality: 1000  
Close enough?: yes

```js
db.many_rows.aggregate([{"$lookup":{"from":"many_rows","localField":"i_idx","foreignField":"i_idx","as":"right1"}},{"$unwind":"$right1"},{"$lookup":{"from":"many_rows","localField":"i_idx","foreignField":"i_idx","as":"right2"}},{"$unwind":"$right2"},{"$match":{"i_idx":1}}])
```
Actual cardinality: 1  
Estimated cardinality: 1  
Close enough?: yes

```js
db.many_rows.aggregate([{"$lookup":{"from":"many_rows","localField":"i_idx","foreignField":"i_idx","as":"right1"}},{"$unwind":"$right1"},{"$lookup":{"from":"many_rows","localField":"i_idx","foreignField":"i_idx","as":"right2"}},{"$unwind":"$right2"},{"$match":{"d_idx":1}}])
```
Actual cardinality: 100  
Estimated cardinality: 100  
Close enough?: yes

## Different join keys
```js
db.many_rows.aggregate([{"$lookup":{"from":"many_rows","localField":"i_idx","foreignField":"i_idx","as":"right1"}},{"$unwind":"$right1"},{"$lookup":{"from":"many_rows","localField":"i_idx_offset","foreignField":"i_idx_offset","as":"right2"}},{"$unwind":"$right2"},{"$match":{}}])
```
Actual cardinality: 1000  
Estimated cardinality: 1000  
Close enough?: yes

```js
db.many_rows.aggregate([{"$lookup":{"from":"many_rows","localField":"i_idx","foreignField":"i_idx","as":"right1"}},{"$unwind":"$right1"},{"$lookup":{"from":"many_rows","localField":"i_idx_offset","foreignField":"i_idx_offset","as":"right2"}},{"$unwind":"$right2"},{"$match":{"i_idx":1}}])
```
Actual cardinality: 1  
Estimated cardinality: 1  
Close enough?: yes

```js
db.many_rows.aggregate([{"$lookup":{"from":"many_rows","localField":"i_idx","foreignField":"i_idx","as":"right1"}},{"$unwind":"$right1"},{"$lookup":{"from":"many_rows","localField":"i_idx_offset","foreignField":"i_idx_offset","as":"right2"}},{"$unwind":"$right2"},{"$match":{"d_idx":1}}])
```
Actual cardinality: 100  
Estimated cardinality: 100  
Close enough?: yes

# Multi-table joins - star
```js
db.many_rows.aggregate([{"$lookup":{"from":"many_rows","as":"right1","let":{"localField":"$i_idx"},"pipeline":[{"$match":{"$expr":{"$eq":["$$localField","$i_idx"]}}}]}},{"$unwind":"$right1"},{"$lookup":{"from":"many_rows","as":"right2","let":{"localField":"$i_idx"},"pipeline":[{"$match":{"$expr":{"$eq":["$$localField","$i_idx"]}}}]}},{"$unwind":"$right2"},{"$match":{}}])
```
Actual cardinality: 1000  
Estimated cardinality: 1000  
Close enough?: yes

```js
db.many_rows.aggregate([{"$lookup":{"from":"many_rows","as":"right1","let":{"localField":"$i_idx"},"pipeline":[{"$match":{"$expr":{"$eq":["$$localField","$i_idx"]}}}]}},{"$unwind":"$right1"},{"$lookup":{"from":"many_rows","as":"right2","let":{"localField":"$i_idx"},"pipeline":[{"$match":{"$expr":{"$eq":["$$localField","$i_idx"]}}}]}},{"$unwind":"$right2"},{"$match":{"i_idx":1}}])
```
Actual cardinality: 1  
Estimated cardinality: 1  
Close enough?: yes

```js
db.many_rows.aggregate([{"$lookup":{"from":"many_rows","as":"right1","let":{"localField":"$i_idx"},"pipeline":[{"$match":{"$expr":{"$eq":["$$localField","$i_idx"]}}}]}},{"$unwind":"$right1"},{"$lookup":{"from":"many_rows","as":"right2","let":{"localField":"$i_idx"},"pipeline":[{"$match":{"$expr":{"$eq":["$$localField","$i_idx"]}}}]}},{"$unwind":"$right2"},{"$match":{"d_idx":1}}])
```
Actual cardinality: 100  
Estimated cardinality: 100  
Close enough?: yes

# Multi-table joins - zero-cardinality tables at various positions
```js
db.no_rows.aggregate([{"$lookup":{"from":"many_rows","localField":"i_idx","foreignField":"i_idx","as":"right1"}},{"$unwind":"$right1"},{"$lookup":{"from":"many_rows","localField":"i_idx_offset","foreignField":"i_idx_offset","as":"right2"}},{"$unwind":"$right2"}])
```
Actual cardinality: 0  
Estimated cardinality: 0  
Close enough?: yes

```js
db.many_rows.aggregate([{"$lookup":{"from":"no_rows","localField":"i_idx","foreignField":"i_idx","as":"right1"}},{"$unwind":"$right1"},{"$lookup":{"from":"many_rows","localField":"i_idx_offset","foreignField":"i_idx_offset","as":"right2"}},{"$unwind":"$right2"}])
```
Actual cardinality: 0  
Estimated cardinality: 0  
Close enough?: yes

```js
db.many_rows.aggregate([{"$lookup":{"from":"many_rows","localField":"i_idx","foreignField":"i_idx","as":"right1"}},{"$unwind":"$right1"},{"$lookup":{"from":"no_rows","localField":"i_idx_offset","foreignField":"i_idx_offset","as":"right2"}},{"$unwind":"$right2"}])
```
Actual cardinality: 0  
Estimated cardinality: 0  
Close enough?: yes

# Multi-table joins - zero-cardinality predicates at various positions
```js
db.many_rows.aggregate([{"$lookup":{"from":"many_rows","as":"right1","let":{"localField":"$i_idx"},"pipeline":[{"$match":{"$expr":{"$eq":["$$localField","$i_idx"]}}}]}},{"$unwind":"$right1"},{"$lookup":{"from":"many_rows","as":"right2","let":{"localField":"$i_idx"},"pipeline":[{"$match":{"$expr":{"$eq":["$$localField","$i_idx"]}}}]}},{"$unwind":"$right2"},{"$match":{"d_idx":-1}}])
```
Actual cardinality: 0  
Estimated cardinality: 0  
Close enough?: yes

```js
db.many_rows.aggregate([{"$lookup":{"from":"many_rows","as":"right1","let":{"localField":"$i_idx"},"pipeline":[{"$match":{"$expr":{"$eq":["$$localField","$i_idx"]},"d_idx":-1}}]}},{"$unwind":"$right1"},{"$lookup":{"from":"many_rows","as":"right2","let":{"localField":"$i_idx"},"pipeline":[{"$match":{"$expr":{"$eq":["$$localField","$i_idx"]}}}]}},{"$unwind":"$right2"}])
```
Actual cardinality: 0  
Estimated cardinality: 0  
Close enough?: yes

```js
db.many_rows.aggregate([{"$lookup":{"from":"many_rows","as":"right1","let":{"localField":"$i_idx"},"pipeline":[{"$match":{"$expr":{"$eq":["$$localField","$i_idx"]}}}]}},{"$unwind":"$right1"},{"$lookup":{"from":"many_rows","as":"right2","let":{"localField":"$i_idx"},"pipeline":[{"$match":{"$expr":{"$eq":["$$localField","$i_idx"]},"d_idx":-1}}]}},{"$unwind":"$right2"}])
```
Actual cardinality: 0  
Estimated cardinality: 0  
Close enough?: yes

# Summary
Good estimations: 54  
Bad estimations: 13  
