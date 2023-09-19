# Query Stats
This directory is the home of the infrastructure related to recording query statistics for the
database. It is not to be confused with src/mongo/db/query/stats/ which is the home of the logic for
computing and maintaining statistics about a collection or index's data distribution - for use by
the query planner.

At the center of everything here is the `QueryStatsStore`, which essentially maps query shapes
(`query_shape::Shape`) to some metrics about how often each one occurs. For example, if the client
does this:
```js
db.example.findOne({x: 24});
db.example.findOne({x: 53});
```
then the `QueryStatsStore` should contain an entry for a single query shape which would record 2
executions and some related statistics (see `QueryStatsEntry` for details).

The query stats store actually uses _more_ dimensions (i.e. more granularity) to group incoming
queries than just the query shape. For example, these queries would all three have the same shape
but the first would have a different query stats store entry from the other two:
```js
db.example.find({x: 55});
db.example.find({x: 55}).batchSize(2);
db.example.find({x: 55}).batchSize(3);
```
There are two distinct query stats store entries here - both the examples which include the batch
size will be treated separately from the example which does not specify a batch size.

The dimensions considered will depend on the command, but can generally be found in the
`KeyGenerator` iterface, which will generate the query stats store keys by which we accumulate
statistics. As one example, you can find the `FindKeyGenerator` which will include all the things
tracked in the `FindCmdQueryStatsStoreKeyComponents` (including batchSize shown in this example).
