# Query Rewrites for Time-Series Collections

For a general overview about how time-series collection are implemented, see [db/timeseries/README.md](../../../db/timeseries/README.md). For sharding specific logic, see [db/s/README_timeseries.md](../../../db/s/README_timeseries.md).
This section will focus on the query rewrites for time-series collections and the `$_internalUnpackBucket`
aggregation stage, and assumes knowledge of time-series collections basics.

# Different Types of Time-Series Queries

## Queries on the view

Because the user-created time-series collection is a view, all queries against it (`find`, `count`, `distinct`
and `aggregate`) are transformed into an aggregation request against the backing system collection with the
`$_internalUnpackBucket` stage prepended to the generated pipeline.

The entrypoint for these aggregate operations is `runAggregate()` and then `runAggregateOnView()`. If
all the validation checks pass, the view is then resolved. A resolved view will contain the original pipeline,
the namespace of the collection underlying the view, and for time-series collections more information about
the buckets collection, such as if the buckets collection uses an extended range (dates pre 1970). The
resolved view is turned into a new aggregation request, where an internal aggregation stage
(`$_internalUnpackBucket`) is added as the first stage in the pipeline (see `asExpandedViewAggregation()`).
Then `runAggregate` is called again on this new request.

## Queries on the buckets collection

Queries directly on the **buckets collection** are executed like non time-series collection
queries. However, prior to PM-3167 (on versions before 7.2), queries on the buckets collection are forced
to run in the classic execution engine. In 7.2+, if the queries are eligible, they will run in SBE.

Similar to non time-series collections, the `$_internalUnpackBucket` stage is not used when executing queries against the buckets
collection, because the buckets collection is not a view. We do not expect users to directly query
the buckets collection, since the buckets collection is made automatically when users create time-series
collections. Also, users should query the view to take advantage of time-series specific optimizations.

# $\_internalUnpackBucket Aggregation Stage Optimizations

The `$_internalUnpackBucket` stage is implemented by `DocumentSourceInternalUnpackBucket` and, like
all other document sources, provides the `doOptimizeAt()` function. This function contains most of the query rewrites
and optimizations specific to time-series. The optimizations contained in this function are listed below. Most of
them aim to limit the number of buckets that need to be unpacked to satisfy the user's query and in some cases
may remove the `$_internalBucketUnpack` stage completely. For example, removing the `$_internalBucketUnpack`
stage and rewriting a `$group` stage has been seen to increase performance by 100x.

Just like all document sources, `DocumentSourceInternalUnpackBucket::doOptimizeAt()` can be called
any number of times during the optimization of a pipeline. Optimizations added should handle being
called repeatedly. Additionally, `DocumentSourceInternalUnpackBucket::doOptimizeAt()` only peeks at the next
stage after `$_internalUnpackBucket`. This might prevent eligible stages from being optimized if there
are stages right after `$_internalUnpackBucket` that cannot be optimized. This is a known limitation in the
classic execution engine.

The descriptions of the optimizations below are summaries and do not list all of the requirements for
each optimization.

## A quick note about the `timeField` and `metaField`

Most of the query rewrites described below will rely on the `timeField` and `metaField`. The user
inputted `timeField` and `metaField` values will be different than what is stored in the buckets collection.
When implementing and testing query optimizations, the rewrites from the user `timeField` and `metaField` values
to the buckets collection fields must be tested.

Let's look at an example with a time-series collection created with these options: `{timeField: t, metaField: m}`.

The `timeField` will be used in the `control` object in the buckets collection. The `control.min.<time field>`
will be `control.min.t`, and `control.max.<time field>` will be `control.max.t`. The values for `t` in the
user documents are stored in `data.t`.

The meta-data field is always specified as `meta` in the buckets collection. We will return documents with the user-specified meta-data field (in this case `m`), but we will store the meta-data field values under
the field `meta` in the buckets collection. Therefore, when returning documents we will need to
rewrite the field `meta` to the field the user expects: `m`. When optimizing queries, we will need to
rewrite the field the user inserted (`m`) to the field the buckets collection stores: `meta`.

## $match on metaField reorder

In 5.0+, a `$match` on the `metaField` that immediately follows the `$_internalUnpackBucket` stage is pushed
before `$_internalUnpackBucket`.

For example, for a time-series collections where the `metaField = tags`:

```
// Query issued by the user:
find({tags: 34})

// Pipeline on the buckets collection after the rewrite:
[
  {match: {meta: {$eq: 34}}},
  {$_internalBucketUnpack...}
]
```

## $sort reorder

In 5.0+, a `$sort` stage on the `metaField` that immediately follows the `$_internalUnpackBucket` stage is pushed
before `$_internalUnpackBucket`.

For example, for a time-series collections where the `metaField = tags`:

```
// Query issued by the user:
find({tags: {$lt: 34}}).sort({tags: 1})

// Pipeline on the buckets collection after the rewrite:
[
  {match: {meta: {$lt: 34}}},
  {$sort: {meta: 1}},
  {$_internalBucketUnpack...}
]
```

## $geoNear rewrites

In 5.1+ a `$geoNear` stage with the `metaField` as the key is pushed before `$_internalUnpackBucket` and
rewritten to a `$geoNearCursor` stage. This rewrite does not apply if there is a filter on measurements or time.

In 5.2+ this rewrite was expanded. The `$geoNear` stage with a measurement field as the key is rewritten
as `FETCH` + `INDEX` with a `$_internalBucketGeoWithin` filter inside the `FETCH` node. This optimization
occurs in `DocumentSourceGeoNear::doOptimizeAt()`.

For example, for a time-series collections where the `metaField = tags`:

```
// Query issued by the user:
aggregate([
  {$geoNear: {
    near: {type: "Point", coordinates: [100.0, 10.0]},
    key: 'tags.loc',
    distanceField: "tags.distance",
  }}
])

// Pipeline on the buckets collection right after the rewrite:
[
  {$geoNearCursor: {
    "parsedQuery" : {
      "meta.loc" : {
        $near: {type: "Point",
        coordinates: [100.0, 10.0]}
      }
    }
  }},
  {"$_internalUnpackBucket" : ...}
]
```

## $limit reorder

In 7.1+ an additional `$limit` stage is added prior to the `$_internalBucketUnpack` stage. This rewrite
reduces the number of fetched buckets since buckets are guaranteed to contain at least 1 measurement. This rewrite
does not apply if there is a filter on measurements or time.

For example,

```
// Query issued by the user:
aggregate([{$limit: 2}])

// Pipeline on the buckets collection right after the rewrite:
[
  {$limit: 2},
  {$_internalBucketUnpack...},
  {$limit: 2}
]
```

## Control-based rewrites for $group

See `DocumentSourceInternalUnpackBucket::rewriteGroupStage()`. This rewrite does not apply if there
is a filter on measurements or time.

In 5.0+, a `$group` stage with the group key on the `metaField` and **only** using `$min`and`$max`
accumulators is rewritten to use the controls from the buckets collection, and the `_internalUnpackBucket`
stage is removed.

In 7.1+, the rewrites for the `$group` stage expanded to include:

1. If the group key is a constant expression
2. If the group key consists of a combination of #1, and references to the `metaField`.
3. If the group uses `{$sum: 1}` accumulator or its equivalent $count accumulator. Note: rewrites for
   sums of other constants aren't currently supported.

In addition to those, for fixed bucket collections there is:

4. If the group key is a `$dateTrunc` expression on the `timeField`. See [fixed bucket collections](#fixed-bucketing-optimizations)
   for more details about fixed bucket collections.

For example, for a time-series collections where the `metaField = tags`:

```
// Query issued by the user:
aggregate([
  {$group: {
    _id: {m1: '$tags.m1', m2: '$tags.m2', m3: 'hello' },
    accmin: {$min: '$val'},
    accmax: {$max: '$val'}
    }
  }
])

// Pipeline on the buckets collection right after the rewrite (post 7.1):
[
  {$group: {
    _id: {m1: '$meta.m1', m2: '$meta.m2', m3: 'hello' },
    accmin: {$min: '$control.min.val'},
    accmax: {$max: '$control.max.val'},
    }
  }
]

// Query issued by the user:
aggregate([{$count: 'foo'}])

// Pipeline on the buckets collection right after the rewrite (post 7.1):
[
  {$group: {
    _id: null,
    foo: {
      $sum: {
        $cond: [
          {$gte: ["$control.version, 2]},
          '$control.count',
          {$size: [{$objectToArray: ['$data.time']}]
      }]}}}}
  }},
  { $project: { foo: true, _id: false }}
]
```

## rewrites for 'count-like' queries

In 5.0+, if the pipeline depends only on the number of documents but not their fields, we update the `include`
parameter in the `$_internalUnpackBucket` stage to `[]`, so the stage will "unpack" and return empty documents.
This rewrite applies for specific accumulators in a `$group` stage, such as `{$sum: 2}`, and `{$sum: 1}`.

To use the previous rewrite, the `$count` stage is rewritten as a `$group` with the `$sum` accumulator
followed by a `$project` stage. We use the `$sum` accumulator, since the `$count` accumulator is just
syntactic sugar for `{$sum: 1}`.

In 7.1+ since the added group stage uses `{$sum: 1}`, the`$group` stage is rewritten and the
`$_internalUnpackBucket` stage is removed (See the previous section on
[control based rewrites for $group](#control-based-rewrites-for-group)). This rewrite does not apply
if there is a filter on measurements or time.

For example, for a time-series collections where the `metaField = tags`:

```
// Query issued by the user:
aggregate([{$count: 'foo'}])

// Pipeline on the buckets collection right after the rewrite (pre 7.1):
[
  {$_internalUnpackBucket: { include: [], timeField: 't', metaField: 'tags'}},
  {$group: { _id: null, foo: {$sum : 1}}},
  { $project: { foo: true, _id: false }}
]
```

## Last-point queries

See `DocumentSourceInternalUnpackBucket::optimizeLastPoint()`. This rewrite does not apply if there
is a filter on measurements or time.

Last-point/first-point queries return the most recent or the earliest measurement per meta value. Last-point queries
must contain both a `$sort` and a `$group` stage with `$first` or `$last` accumulators, or only a `$group` stage with `$top`, `$topN`,
`$bottom`, or `$bottomN` accumulators. A last-point queries could look like:

```
{$sort: {"meta": 1, “time”: 1}},
{$group: {_id: “meta.a”, “last”: {$last: “$time"}}}])
```

In 6.0+, there are two optimizations:

1. Add a bucket-level `$sort` and `$group` stages before `$_internalUnpackBucket`. The new `$sort` and
   `$group` stages use bucket-level fields such as `meta`, and the `control` fields.
2. Replace the `$group` with a `DISTINCT_SCAN` and `$groupByDistinctScan`. This requires a matching index
   to exist. For example, last-point queries on indexes with descending time and `$top/$first` accumulators
   are eligible for a `DISTINCT_SCAN`. However, last-point queries with an ascending time index and
   `$last/$bottom` accumulators are not eligible for a `DISTINCT_SCAN` unless there is an additional
   secondary index.

For example, for a time-series collections where the `metaField = tags`:

```
// Query issued by the user:
aggregate([
  {$sort: {'tags.a': 1, time: -1}},
  {$group: {_id: '$tags.a', b: {$first: '$b'}}
])

// Pipeline on the buckets collection right after the rewrite (optimization 1):
[
  {$sort: {'meta.a': 1, 'control.max.time': -1, 'control.min.time': -1}},
  {$group: {
    _id: '$meta.a',
    meta: {$first: '$meta'},
    control: "{$first: '$control'},
    data: {$first: '$data'}
  }},
  {$_internalUnpackBucket: ...},
  {$sort: {'tags.a': 1, time: -1}},
  {$group: {_id: '$tags.a', b: {$first: '$b'}}}
]
```

## Control-based rewrites for $match

See `BucketSpec::createPredicatesOnBucketLeveField()`.

### $match on measurements

See `BucketLeveComparisonPredicateGenerator::generateNonTimeFieldPredicate()`.

In 6.0+ a `$match` stage on a measurement field triggers an additional filter in the `COLLSCAN` or
`FETCH` against the `control` fields in the buckets. For example, the query `find({measurement0: 42})`
(`measurement0` is not the `timeField` nor the `metaField`) generates the following filter in the `COLLSCAN`
or `FETCH` stage.

```
"$and" : [
  {"control.max.measurement0" : {"$_internalExprGte" : 42}},
  {"control.min.measurement0" : {"$_internalExprLte" : 42}}
]
```

There are similar optimizations for the rest of the comparison operators (`$gt`, `$gte`, `$lt`, `$lte`).

We can perform this optimization, since the control fields are guaranteed to hold the minimum and maximum value
of all fields in the bucket collections. In this example, documents with `measurement0=42` must be in buckets where
42 is in between `control.max.measurement0` and `control.min.measurement0`.

This optimization speeds up the queries, because the filter in the `COLLSCAN` prevents completely
irrelevant buckets from being returned, unpacked, and then the individual events filtered by the
predicate.

### $match on timeField

See `BucketLeveComparisonPredicateGenerator::generateTimeFieldPredicate()`.

In 6.0+ if the `$match` stage is on the `timeField`, there are additional predicates added to the generated
filter. In addition to the control-based filters mentioned above, filters on the `_id` are generated.
The bucket `_id` fields incorporate the minimum timestamp of the bucket. The addition of the `_id` predicates
enables the query to use a bounded `CLUSTERED_IXSCAN`.

### wholeBucketFilter with $match on timeField

In 6.0+ the predicates generated for a `$match` on the `timeField` are not applied to the unpacked documents if the
entire bucket matches the predicate. The generated predicates are stored as a `wholeBucketFilter` and
an `eventFilter`. If the bucket document fits the `wholeBucketFilter` then the individual measurements
do not need to be filtered by the `eventFilter`. Only if the bucket does not fit the `wholeBucketFilter`
then the `eventFilter` is applied to each unpacked document.

This optimization only occurs if the value in a `$match` is of type `BSONDate` or on the `metaField`.

For example, a query like `({time: {$lt: Date('2022-01-1')}})`, would produce the following filters:

```
wholeBucketFilter: {control.max.time: {$lt: Date('2022-01-1')}}
eventFilter: {time: {$lt: Date('2022-01-1')}}
```

### Fixed bucketing optimizations

In 7.1+ there are two optimizations that apply if the buckets collection has fixed buckets. A collection
has fixed buckets if the collection was created with `bucketMaxSpanSeconds` and `bucketRoundingSeconds`,
and those parameters have not been changed. For more details about these parameters, see
[db/timeseries/README.md](../../../db/timeseries/README.md#bucketing-parameters)

1. The `eventFilter` can be removed if the predicates use `$gte` and `$lt`, is on the `timeField`, and the
   predicate aligns with the bucket boundaries. This optimization allows further optimizations to apply.
2. The generated predicate on the `timeField` is tighter than non fixed bucket collections.
   See `FixedBucketsLevelComparisonPredicateGenerator::generateTimeFieldPredicate()`.

For example, if `bucketMaxSpanSeconds = 3600`, a query like `({time: {$lt: Date('2022-01-1T01:00:00Z')}})`
would not generate an `eventFilter` and `wholeBucketFilter`. However, a query like `({time: {$lt: Date('2022-01-1T01:12:00Z')}})`
would generate the following `eventFilter` and `wholeBucketFilter`:

```
eventFilter: {"time": $lt: Date('2022-01-1T01:12:00Z')}
wholeBucketFilter: {"control.max.time": $lt: Date('2022-01-1T01:12:00Z')}
```

## $project, $addStage, and $set rewrite

In 5.0+, there are a few rewrites with `$project` and `$addFields`:

1. Simple projections, like `{_id: 0, x: 1, y: 1}` are internalized into the `$_internalUnpackBucket` stage using
   the `include` or `exclude` parameters.
2. `$project`, `$addFields` and `$set` that reference the `metaField` are pushed before the `$_internalUnpackBucket` stage.
   This rewrite does not occur if there is a filter on measurements or time.

For example,

```
// Rewrite #1:
// Query issued by the user:
aggregate([{$project: {x: true, y: true}}])

// Pipeline on the buckets collection right after the rewrite:
[
  {$_internalUnpackBucket: {
    include: ['_id', 'x', 'y'], timeField: "time", bucketMaxSpanSeconds: 3600
    }
  }
]

// Rewrite #2 (the metaField in this example is 'tags'):
// Query issued by the user:
aggregate([{$addFields: {newMeta: {$toUpper : '$tags'}}}])

// Pipeline on the buckets collection right after the rewrite:
[
  {$addFields: {newMeta: {$toUpper: ['$meta']}}},
  {$_internalUnpackBucket: {
    exclude: [], timeField: 'time', metaField: 'tags', computedMetaProjFields: ['newMeta']
  }}
]
```

## Streaming $group

See `DocumentSourceInternalUnpackBucket::enableStreamingGroupIfPossible()`.

In 6.0+, the `$group` stage will be replaced with `$_internalStreamingGroup` when the group key is monotonic
on time and the documents are sorted on time. This rewrite will process the groups in batches. See
`DocumentSourceStreamingGroup` for more details.

For example,

```
// Query issued by the user:
aggregate(
  [
    {$sort: {time: 1, measurement: 1}},
    {$group: {
      _id: "$time",
      average_price: {$avg: {$multiply: ["$price", "$amount"]}}
    }}
  ])

// Pipeline on the buckets collection right after the rewrite:
[
  {$_internalUnpackBucket: ...},
  {$sort: {
    sortKey: {time: 1, measurement: 1},
  }},
  {$_internalStreamingGroup: {
    _id: "$time",
    average_price:  {$avg: {$multiply: ["$price", "$amount"]}},
    monotonicIdFields: ["_id"]
  }}
]
```

# Other Optimizations

The following rewrites are implemented outside of `DocumentSourceInternalUnpackBucket::doOptimizeAt()`:

## $sample rewrites

See `PipelineD::buildInnerQueryExecutorSample()`.

In 5.0+, the `$sample` stage will be pushed before the `$_internalUnpackBucket` stage and generate either one of the following plans:

1. The `$cursor` stage with `TRIAL` + `QUEUED_DATA`. This uses the `SAMPLE_FROM_TIMESERIES_BUCKET` stage that
   returns data via the `QUEED_DATA` stage. The `SAMPLE_FROM_TIMESERIES_BUCKET` stage uses the ARHASH
   algorithm [see here](https://dl.acm.org/doi/10.1145/93605.98746).

2. The `$cursor` stage with`TRIAL` + `UNPACK_BUCKET` + `COLLSCAN` + `$sample`. This uses a top-k sorting
   approach.

Both plans will remove the `$_internalUnpackBucket` stage from the pipeline.

During development it was shown experimentally the rewrite should not apply if
the sample size is greater than 1% of the maximum possible number of measurements in the collection (`numBuckets * maxMeasurementsPerBucket`).

To decide between option 1 and 2 we run a trial using the `TRIAL` stage between `SAMPLE_FROM_TIMESERIES_BUCKET`
and `UNPACK_BUCKET`. Generally, option 1 will be chosen when buckets are fuller.

For example,

```
// Query issued by the user:
aggregate([{$sample: {size: 20}}])

// Let's say we have 40 buckets and 100+ measurements per bucket. We will choose option 1.
// The explain output of the winning query plan would look like:
"winningPlan" : {
  "stage" : "TRIAL",
  "inputStage" : {
    "stage" : "QUEUED_DATA"
    }
}
```

## $\_internalBoundedSort on the timeField

See `DocumentSourceSort::createBoundedSort()` inside `PipelineD::buildInnerQueryExecutorGeneric()`.

In 6.0+, `$sort` is replaced by `$_internalBoundedSort` if possible when sorting in ascending or descending
order on the `timeField`, or on `metaField` and `timeField`. The new query plan generated by this
optimization has two steps:

1. Use a clustered index or a secondary index on the `timeField` to scan the buckets in time order.
2. After unpacking, interleave the events from any buckets that overlap. This is done by `$_internalBoundedSort`.

This query plan traverses the buckets in time order. With this traversal, it is guaranteed that when we process
a bucket with a minimum timestamp `t` (`control.min.timeField = t`), all of the measurements from now
on will have a timestamp equal to or greater than `t`. Therefore, we can return all the measurements we
have sorted with a timestamp less than `t`. So, `$_internalBoundedSort` is faster and less resource
intensive than `$sort`.

Some queries require users to use a `hint` for the query planner to pick the correct index that supports
bounded sort. For example, `{$sort: {meta: 1, time: 1}}` requires the user to pass in the `hint` `{meta: 1, time: 1}`.

This optimization is blocked if after unpacking the `timeField` or the `metaField` are not preserved
in the measurement docs, or if the fields in the `$sort` are modified by a projection.

```
// Query issued by the user:
aggregate([{$sort: {time: 1}}])

// Pipeline on the buckets collection right after the rewrite:
[
  {$_internalUnpackBucket: ...},
  {$_internalBoundedSort: {
    sortKey: {time: 1},
    bound: {base: min, offsetSeconds: 0},
    limit: 0
  }
]

// Query issued by the user:
aggregate([{$sort: {meta: 1, time: 1}}, {hint: {meta: 1, time: 1}}])

// Pipeline on the buckets collection right after the rewrite:
[
  {$_internalUnpackBucket: ...},
  {$_internalBoundedSort: {
    sortKey: {meta: 1, time: 1},
    bound: {base: min, offsetSeconds: 0},
    limit: 0
  }
]
```

# Lowering pipelines to the Slot Based Execution Engine

Note there are active projects making changes to what is written below. This section will be updated as the projects finish.

## When pipelines are lowered to SBE

At the time of writing, to be lowered to SBE a pipeline on a time-series collection must:

1. Limit the unpacked fields to a statically known set (must have the `includes` parameter in the unpack stage).
2. Not include a `$sort` stage on the `timeField`.
3. Not implement the last point optimization.
4. Not use event-filters with expressions unsupported in SBE.
5. Not use filters on the `metaField` with expressions unsupported in SBE.

If the pipeline meets all of these requirements, its prefix up to a stage that isn't supported by SBE
will be lowered to SBE. If all of the stages are supported in SBE, the entire pipeline will run in SBE.

If the pipeline fails to meet any of the requirements above, the pipeline will fully run in the classic engine.

## What happens when pipelines are lowered to SBE

When a pipeline is lowered to SBE, the query plan will have a `UNPACK_TS_BUCKET` node instead of the
`$_internalUnpackBucket` aggregation stage. The `UNPACK_TS_BUCKET` node uses two SBE stages
(`TsBucketToCellBlockStage`, and `BlockToRowStage`) to transform a single bucket into a set of slots
that can be used as input to other SBE stages.

The `TsBucketToCellBlockStage` takes in a bucket as a BSON document, selects the top-level fields
from the bucket’s `data` field and produces `CellBlock` values. A `CellBlock` contains all of the
values at a certain field path. Some operations can be performed on these blocks as a whole in a
vectorized manner. This process is called block processing, and it greatly improves the performance of queries
over time-series collections. Currently, block processing supports the whole bucket filter and event filters.

The `BlockToRowStage` is not specific to time-series. It takes in a block of values, which can be
`CellBlock` values. These values might have been already processed by block processing, and therefore
the values inside the `CellBlock` might have changed from the initial data in the bucket. This stage produces
a set of slots, where each slot at a given time holds a single value from the source `CellBlock`s.
We'll illustrate this with an example.

**Example:**

Let's suppose, `TsBucketToCellBlockStage` produces two slots containing the following blocks:

```
block for path "a" is in slot s1: {1, 2, 3, 4, 5}
block for path "b" is in slot s2: {10, 20, 30, 40, 50}
```

From these inputs and with no filters, `BlockToRowStage` will produce two slots (`s3` and `s4`) that will
be populated with one value from each of the source blocks at a time. For this example, we will look
at the different states of the execution engine (labeled t1, t2, etc...) and the values in slots `s3` and `s4`.

| States             | t1  | t2  | t3  | t4  | t5  |
| ------------------ | --- | --- | --- | --- | --- |
| value in slot `s3` | 1   | 2   | 3   | 4   | 5   |
| value in slot `s4` | 10  | 20  | 30  | 40  | 50  |

The slots `{s3, s4}` are often referred to as a "row" or the "row representation".
Other SBE stages that process individual values can execute on slots `s3` and `s4`.

# References

See:
[MongoDB Blog: Time Series Data and MongoDB: Part 2 - Schema Design Best Practices](https://www.mongodb.com/blog/post/time-series-data-and-mongodb-part-2-schema-design-best-practices)
