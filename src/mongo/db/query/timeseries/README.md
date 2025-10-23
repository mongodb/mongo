# Query Rewrites for Timeseries Collections

For a general overview about how timeseries collection are implemented, see [db/timeseries/README.md][db readme]. For sharding
specific logic, see [db/global_catalog/README_timeseries.md][catalog readme]. This document will focus on query translations and
optimizations for timeseries collections and the `$_internalUnpackBucket` aggregation stage, and assumes knowledge of timeseries
collections basics. We perform timeseries rewrites before and during optimizations. For clarity in this README and all query timeseries
resources, when we are performing pre-optimization rewrites we will use the term _translations_, and we will use the term _rewrites_
for timeseries optimizations.

There are two different types of timeseries collections.

1. Legacy/viewful. Supported on all server versions below 9.0.
2. Viewless. Projected to be supported on server versions 9.0+.

## Pre 9.0: Legacy timeseries collections

These timeseries collection have 2 namespaces that are automatically made when the collection is created. The user defined namespace
will be a view, and `system.buckets.<namespace>` will store the timeseries documents in bucket document format. More details about the
buckets can be found in [db/timeseries/README.md][db readme].

### Queries on the view

Because the user-created timeseries collection is a view, all queries against it (`find`, `count`, `distinct`
and `aggregate`) are transformed into an aggregation request against the backing `system.buckets.<namespace>` collection with the
`$_internalUnpackBucket` stage prepended to the generated pipeline.

The entrypoint for these aggregate operations is `runAggregate()` and then `runAggregateOnView()`. If
all the validation checks pass, the view is then resolved. A resolved view will contain the original pipeline,
the namespace of the collection underlying the view, and for timeseries collections more information about
the buckets collection, such as if the buckets collection uses an extended range (dates pre 1970). The
resolved view is turned into a new aggregation request, where an internal aggregation stage
(`$_internalUnpackBucket`) is added as the first stage in the pipeline (see `asExpandedViewAggregation()`).
Then `runAggregate` is called again on this new request.

### Queries on the buckets collection

Queries directly on the **buckets collection** are executed like non timeseries collection
queries. However, prior to PM-3167 (on versions before 7.2), queries on the buckets collection are forced
to run in the classic execution engine. In 7.2+, if the queries are eligible, they will run in SBE.

Similar to non timeseries collections, the `$_internalUnpackBucket` stage is not used when executing queries against the buckets
collection, because the buckets collection is not a view. We do not expect users to directly query
the buckets collection, since the buckets collection is made automatically when users create timeseries
collections. Also, users should query the view to take advantage of timeseries specific optimizations.

## 9.0+: Viewless timeseries collections

<!-- TODO SERVER-102458 Make any necessary changes for post 9.0 timeseries cleanup. -->

Unlike viewful timeseries collections, these collections have a **single** namespace, which the user defines. Therefore, there
is no `system.buckets.<namespace>` collection and there is no view to resolve when querying viewless timeseries collections.

### Queries that return user documents

> [!NOTE]
> This is the expected workflow for users.

Like with queries against viewful timeseries collections, queries against viewless timeseries collections are also transformed
into aggregation requests, since we still must prepend the `$_internalUnpackBucket` aggregation stage. `find`, `count`, and `distinct`
will all check if the collection is timeseries by checking the collection catalog data (either `CollectionOrViewAcquisition` for the
shard role, or `CollectionRoutingInfo` for the router role). If the collection is a timeseries collection, we will translate the command
as an aggregation request.

In the `aggregate` command, we also check if the collection is timeseries by checking the collection catalog data. During aggregation,
after any views are resolved and the pipeline is parsed and validated, we will use the collection catalog data in
[performPreOptimizationRewrites][pre op rewrites] to prepend the `$_internalUnpackBucket` stage. The pipeline will then set
`_translatedForViewlessTimeseries` to true, since the timeseries translation **can only happen once** during the lifetime of a pipeline.

### Queries that return raw buckets

> [!NOTE]
> This is not the expected workflow for users.

Unlike viewful timeseries collections, we cannot query `system.buckets.<namespace>` directly because the namespace does not exist.
To maintain this functionality, queries directly on the buckets use the same namespace as the timeseries collection and set the `rawData`
field on the command object to `true`. Therefore, queries on `<namespace>` with `rawData = true` will return the same results as
queries on `system.buckets.<namespace>`.

### Considerations when working with viewless timeseries

These were important lessons taken from SPM-4217, which added aggregation support for viewless timeseries.

1. Detecting if a collection is timeseries now requires accessing collection catalog data. The collection catalog must be up to date,
   since the correctness of timeseries queries depend on detecting that the collection is timeseries. Therefore, we recommend using the
   shard role and the router role APIs to ensure the data from the catalog is up to date. Additionally, when accessing the catalog we must
   consider all 3 scenarios:

   - **Tracked collections**. If we are acting as a router, we can use `CollectionRoutingInfo` retrieved by a `RoutingContext`
     (see [sharded_agg_helpers::finalizeAndMaybePreparePipelineForExecution][finalize func] for an example).
   - **Untracked collections**. These are unsharded collections that live on the primary shard. The config server does not have
     information about this collection, so we must contact the primary shard to check if the collection is timeseries.
   - **Local collections**. If we are in a shard role or can perform a local read, we can use the local catalog. These can be
     unsharded collections that live on that shard, or all collections in a non sharded cluster.

2. We must consider how an aggregation stage or command should work when `rawData = true`. For example, `$out` errors if `rawData = true`
   because `$out` cannot work on raw buckets ([code link][out]).
3. Two `StageConstraint`s are important for defining timeseries behavior:

   - `canRunOnTimeseries` should be set to `false` if the stage should error when run on a timeseries collection. For example,
     `$search` can never run on a timeseries collection ([code link][search]).
   - `consumesLogicalCollectionData` should be set to `false` if the stage processes collection metadata, internally created data such as
     oplog entries, or has no input. If the first stage of the pipeline sets `consumesLogicalCollectionData` to `false`, then the
     `$_internalUnpackBucket` stage will not be prepended ([code link][ts translation]). Examples are `$queue` and `$collStats`
     ([code link][collstats]).

## How collMod affects timeseries queries

Users can run a `collMod` command at anytime, which can only increase the granularity value for timeseries collections. Increasing the
granularity increases the value of `bucketMaxSpanSeconds`, which means that new buckets will span more time. Buckets already created
do not change. `bucketMaxSpanSeconds` is used to push down `$match` predicates and target shards if the shard key is on `timeField`
(which is a deprecated feature). But this value can change! A query that was already running when the `collMod` command was issued
can use an older value of `bucketMaxSpanSeconds` and miss new writes during the aggregation (see
[bucket_unpacking_with_sort_granularity_change.js][granularity test]).

Similarly, a query can use an older value of granularity when targeting shards. If we are targeting shards with the old granularity
value, we might miss buckets made with the new granularity value, which is acceptable if the query started before the `collMod` command.

Conversely, if the query targets shards using the new granularity value, then our query predicate would span more time (since granularity
can only increase), so we will target more shards than before. We can illustrate this with a simplified example (for more details see
[sharding timeseries README][catalog readme]):

To ensure we capture all relevant buckets we expand our query predicate by the value of`bucketMaxSpanSeconds`. So if `bucketMaxSpanSeconds`
is `1 minute` and the query predicate is time equals 5:01pm, we will add and subtract `1 minute` to the query predicate and retrieve all
buckets that hold measurements between 5:00-5:02pm. If `bucketMaxSpanSeconds` increases to `1 hour`, we will add and subtract `1 hour` in
the query predicate to retrieve buckets with measurements between 4:00 - 6:00pm. This wider query predicate could require contacting more
shards. During the `$_internalUnpackBucket` stage we will filter out all measurements that don't match the predicate.

# Query optimizations for timeseries

The `$_internalUnpackBucket` stage is implemented by `DocumentSourceInternalUnpackBucket` and, like
all other document sources, provides the `doOptimizeAt()` function. This function contains most of the query rewrites
and optimizations specific to timeseries. The optimizations contained in this function are listed below. Most of
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

Let's look at an example with a timeseries collection created with these options: `{timeField: t, metaField: m}`.

The `timeField` will be used in the `control` object in the buckets collection. The `control.min.<time field>`
will be `control.min.t`, and `control.max.<time field>` will be `control.max.t`. The values for `t` in the
user documents are stored in `data.t`.

The meta-data field is always specified as `meta` in the buckets collection. We will return documents with the user-specified meta-data field
(in this case `m`), but we will store the meta-data field values under
the field `meta` in the buckets collection. Therefore, when returning documents we will need to
rewrite the field `meta` to the field the user expects: `m`. When optimizing queries, we will need to
rewrite the field the user inserted (`m`) to the field the buckets collection stores: `meta`.

## $match on metaField reorder

In 5.0+, a `$match` on the `metaField` that immediately follows the `$_internalUnpackBucket` stage is pushed
before `$_internalUnpackBucket`.

For example, for a timeseries collections where the `metaField = tags`:

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

When swapping a `$sort` and a `$_internalUnpackBucketStage`, the `$sort` may have absorbed a `$limit` from the rest of the pipeline. In this case,
we can swap the `$sort` (and its absorbed `$limit`) with the `$_internalUnpackBucketStage`, and we must also add a new `$limit` stage after the
`$_internalUnpackBucketStage`. This reduces the number of fetched buckets and ensures that this limit is respected after the buckets have been unpacked.
This is similar to the logic we follow when we see a standalone `$limit` stage; see the "`$limit` reorder section."

For example, for a timeseries collections where the `metaField = tags`:

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

For example, for a timeseries collections where the `metaField = tags`:

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

For example, for a timeseries collections where the `metaField = tags`:

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
      }]}}
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

For example, for a timeseries collections where the `metaField = tags`:

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
`$bottom`, or `$bottomN` accumulators. A last-point query could look like:

```
{$sort: {"meta": 1, “time”: 1}},
{$group: {_id: “meta.a”, “last”: {$last: “$time"}}}
```

In 6.0+, there are two optimizations:

1. Add a bucket-level `$sort` and `$group` stages before `$_internalUnpackBucket`. The new `$sort` and
   `$group` stages use bucket-level fields such as `meta`, and the `control` fields.
2. Replace the `$group` with a `DISTINCT_SCAN` and `$groupByDistinctScan`. This requires a matching index
   to exist. For example, last-point queries on indexes with descending time and `$top/$first` accumulators
   are eligible for a `DISTINCT_SCAN`. However, last-point queries with an ascending time index and
   `$last/$bottom` accumulators are not eligible for a `DISTINCT_SCAN` unless there is an additional
   secondary index.

For example, for a timeseries collections where the `metaField = tags`:

```
// Query issued by the user:
aggregate([
  {$sort: {'tags.a': 1, time: -1}},
  {$group: {_id: '$tags.a', b: {$first: '$b'}}}
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
[db/timeseries/README.md][db readme params]

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

## Top-k Sort Optimization

See `DocumentSourceGroup::tryToAbsorbTopKSort()`

In 8.0+, if a `$sort` is followed by `$group` with `$first`/`$last`, the `$sort` is removed from the pipeline and
`$first`/`$last` are rewritten as `$top`/`$bottom`, using the same sort key as the `$sort`. This avoids the blocking
top-level sort and instead only sorts for the first and last elements within each group.

The optimization is possible as long as:

1. There is no limit on the `$sort`.
2. The `$sort` is not sorted on the meta field (the [sort reorder](#sort-reorder) is applied instead).
3. There aren't any other accumulators on the `$group` that depend on the ordered input from the sort. Specifically,
   `$firstN` and `$lastN` are unsupported. Accumulators that don't rely on the sort order are allowed.
4. `$_internalUnpackBucket` is directly followed by a `$sort` and a `$group` (this is not a logical limitation but is
   done to reduce complexity).

The optimization still works with compound group keys and compound sort keys.

If the query is also a last-point query, this optimization may be made along with the
[last-point optimization](#last-point-queries), if there is a `$sort` and `$group` that meet the requirements.

For example:

```
// Query issued by the user:
aggregate([
  {$sort: {time: 1}},
  {$group: {
    _id: "$meta",
    first: {$first: "$measurement"},
    last: {$last: "$measurement"}
  }}
])

// Pipeline on the buckets collection after the rewrite:
[
  {$_internalUnpackBucket: {...}},
  {$group: {
    _id: "$meta",
    first: {$top: {sortBy: {time: 1}, output: "$measurement"}},
    last: {$bottom: {sortBy: {time: 1}, output: "$measurement"}}
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

At the time of writing, to be lowered to SBE a pipeline on a timeseries collection must:

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
over timeseries collections. Currently, block processing supports the whole bucket filter and event filters.

The `BlockToRowStage` is not specific to timeseries. It takes in a block of values, which can be
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
[MongoDB Blog: Time Series Data and MongoDB: Part 2 - Schema Design Best Practices][mongo blog]

<!-- Links -->

[pre op rewrites]: https://github.com/mongodb/mongo/blob/1da17948466e00a7a7e1b99b7e1f722bbac66f32/src/mongo/db/pipeline/pipeline.h#L320-L326
[finalize func]: https://github.com/mongodb/mongo/blob/1da17948466e00a7a7e1b99b7e1f722bbac66f32/src/mongo/db/pipeline/sharded_agg_helpers.h#L244
[out]: https://github.com/mongodb/mongo/blob/42e3fa54f9aba2551c62d00cb83f774e66a86c50/src/mongo/db/pipeline/document_source_out.cpp#L137
[search]: https://github.com/mongodb/mongo/blob/42e3fa54f9aba2551c62d00cb83f774e66a86c50/src/mongo/db/pipeline/search/document_source_internal_search_mongot_remote.h#L73
[ts translation]: https://github.com/mongodb/mongo/blob/42e3fa54f9aba2551c62d00cb83f774e66a86c50/src/mongo/db/query/timeseries/timeseries_translation.cpp#L153-L156
[collstats]: https://github.com/mongodb/mongo/blob/42e3fa54f9aba2551c62d00cb83f774e66a86c50/src/mongo/db/pipeline/document_source_coll_stats.h#L142
[granularity test]: https://github.com/mongodb/mongo/blob/a2b4c8032cba1955a0bd4fcfe2125bcb0e11f7de/jstests/noPassthrough/query/timeseries/bucket_unpacking_with_sort_granularity_change.js
[catalog readme]: ../../../db/global_catalog/README_timeseries.md
[db readme]: ../../../db/timeseries/README.md
[db readme params]: ../../../db/timeseries/README.md#bucketing-parameters
[mongo blog]: https://www.mongodb.com/blog/post/timeseries-data-and-mongodb-part-2-schema-design-best-practices
