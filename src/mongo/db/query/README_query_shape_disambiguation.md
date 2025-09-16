# Disambiguation of Various Query Shape Concepts

You have probably arrived here while reading or thinking about one query shape concept and are wondering how it relates to other similar concepts. This page aims to describe the different purposes and discriminating qualities of the following:

- Query Shape
- Query Stats Key
- Plan Cache Key (Classic or SBE)
- Hashes of the above (`queryShapeHash` and friends).

## Summary

| Concept                            | Primary Purpose                                                                                              | More Details                                                                                                                                                               |
| ---------------------------------- | ------------------------------------------------------------------------------------------------------------ | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Query Stats Store Key              | Capture the 'shapified' raw request which resembles what the user typed.                                     | Includes the whole client-facing command interface.                                                                                                                        |
| Query Shape                        | Just the semantically important bits of the query stats key.                                                 | Also used as the key when applying query settings. More details about the boundary with query stats key follow below.                                                      |
| `queryShapeHash`                   | SHA256 applied to the query shape BSON.                                                                      | Shows up in slow query logs, and can be used to set persistent query settings. This last use case is why we use SHA256 so that it is stable across versions/architectures. |
| `CanonicalQuery::QueryShapeString` | The shape of the query that the access planner sees.                                                         | This is based off an optimized and potentially a partial view of the user's query (e.g. it is missing trailing agg stages).                                                |
| `planCacheShapeHash`               | Hash of the `CanonicalQuery::QueryShapeString`.                                                              | Unlike the indexability stuff below, this should be stable as the catalog evolves.                                                                                         |
| `queryHash`                        | Same as the above.                                                                                           | Legacy, preserved for compatibility reasons but now confusing given `queryShapeHash`.                                                                                      |
| Indexability Discriminators        | Separate cache entries for similarly shaped queries which wouldn't qualify for the same indexes.             | For example, a partial index with a predicate `{a: {$gte: 10}}` means that only certain inequalities could use this index.                                                 |
| Plan Cache Key                     | Ensure that queries which are eligible to use the same plan can do so.                                       | A combination of the `QueryShapeString` and indexability discriminators.                                                                                                   |
| `planCacheKeyHash`                 | A hash combination of the planCacheShapeHash and the indexability discriminators, using boost::hash_combine. | Just has to be stable for the life of the process, unlike query settings.                                                                                                  |

## Read More About Each

- [Query Stats README](/src/mongo/db/query/query_stats/README.md)
- [Query Shape README](/src/mongo/db/query/query_shape/README.md)
- [Plan Cache README](/src/mongo/db/query/plan_cache/README.md)

## An Example

Consider a query like the following.

```js
db.runCommand({
  aggregate: "foo",
  pipeline: [
    {$match: {x: {$gte: 2}}},
    {$replaceWith: "$subDoc"},
    {$out: "bar"},
  ],
  comment: "I am just a humble example",
  readConcern: {level: "majority"},
});
```

Let's use this example to draw a contrast between the concepts which are observable via diagnostics.

> Note: when writing this example I chose `$replaceWith` as an example of a stage which cannot be
> pushed into the access planner (the classic engine is not planned to ever support `$replaceWith`,
> and SBE does not at time of this writing). There are plans to support this in SBE at which point
> this example will become stale, but at least for quite some time there will remain some stages which
> are unsupported for push down into the access plan, and the general theme/concept still holds.

### The `planCacheShapeHash` (The Artist Formerly Known as `queryHash`)

For this query, the `planCacheShapeHash` will just consider the namespace and the $match predicate.
It would be the same shape or at least very similar to a find command with an `{x: {$gte: 10}}`
filter, since the access planner won't see/consider`$replaceWith` and everything after that.

### The `planCacheKeyHash`

As noted above, this diagnostic will include discriminators. A hypothetical partial index
`{x: {$gte: 5}}` would create the need to separate our `{x: {$gte: 2}}` example predicate from
similar `{x: {$gte: 10}}` predicates. The former could not use the index since there might be
missing data. You could think of this like hash combining with a bitmap representing which of a
possible 64 indexes might be eligible for this query, though the details are likely different.

### The `queryShapeHash`

This diagnostic will compute the query shape for the entire aggregate command, including the
`$replaceWith` stages. Notably, it will not include the `comment` or the `readConcern`. Read on for
why.

### The Query Stats Store Key

This doesn't show up in slow query logs, but it does show up in `$queryStats` output, so it doesn't
have a simple diagnostic name like the others. This will shapify the whole command including the
`comment` and `readConcern`.

## Query Stats vs. Query Shape: Which Options Go Where?

There are several things to keep in mind when deciding this question:

1. The Query Stats Store Key is generally meant to be the most discriminating way to collect metrics.
   It is always possible for consumers of the metrics to perform their own grouping operation to
   collapse two or more groups back into one, but it is impossible to undo a grouping. That being said,
   we also cannot afford to have infinite entries, so there is a balance. As an example, we do not want
   to track each and every 'comment' separately, since we know of cases where customers may use the
   comment field as a sort of request ID with very high cardinality.

2. The Query Shape is the key used for query settings application. Any two queries with the same
   shape will have the same settings applied. It would then logically follow that any two queries with
   the same query stats store key also have the same query settings applied, since the query shape
   is part of the query stats store key.

3. The Query Shape is generally meant to capture anything semantically important to the query. If an
   option may change the results, it should probably go here. If the option might only impact
   performance or isolation, it should not go here. A couple examples:

   - The 'maxTimeMs' is not part of the query shape since it does not matter for the semantics of the
     query - it's purely an operational concern.
   - As a trickier example, 'readConcern' was also excluded from the query shape since it only impacts
     isolation guarantees. A shapified version is included in the query stats store key. From a query
     language perspective, it does not really dictate which documents will semantically match the query -
     it's purely a matter of timing. It was deemed more of an operational option/concern. This is a close
     call, because - considering shard filtering and the readConcern level 'available', which does not
     apply shard filtering - a different readConcern level may actually impact which query plan is
     appropriate, and so it probably should play a discriminating role in a plan cache key. A key thought
     experiment here was that a query setting for two identical queries which differ only by readConcern
     should probably apply to both.
   - Finally, another borderline example, the 'hint' option is **not** part of the query shape because
     it should only impact performance. The 'hint' is part of the query stats store key. Further, the
     team decided that a query setting should likely impact all queries which differ only by hint (and it
     should override that hint). If an operator sees that a particular query shape should prefer one
     index type, it should logically apply to all queries of that shape. One day we may add the ability
     to override a query setting via a hint, but this is the intended behavior for now.
