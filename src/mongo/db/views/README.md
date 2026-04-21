# Views

A [view](https://www.mongodb.com/docs/manual/core/views/) is a virtual collection defined by an
aggregation query on a collection or another view. Views effectively act as a shortcut to the result
of a pre-defined aggregation pipeline. See
[`ViewDefinition`](https://github.com/mongodb/mongo/blob/e16bc2248a3410167e39d09bb9bc29a96f026ead/src/mongo/db/views/view.h#L46).

MongoDB currently only supports read-only, _non-materialized_ views. This means the underlying
aggregation pipeline defining a view must be re-executed for every query. There is a long-standing
request ([SERVER-27698](https://jira.mongodb.org/browse/SERVER-27698)) to add support for
_materialized_ views, where the view itself would be stored to avoid re-executing its pre-defined
aggregation pipeline on each query. Note that in a materialized view, changes in the underlying
collection are not reflected in successive calls on the view because its contents are stored on
creation.

For example, you may want to create a view that excludes any personally identifiable information
(PII) from your collection, thereby ensuring anonymity for users. Here is one such document in the
`students` collection:

```
{
  "name": "Alice Smith",
  "email": "alice.smith@example.com",
  "major": "Computer Science",
  "GPA": 3.8,
  "graduationYear": 2025
}
```

Now we can create a view called `anonStudents2025` on the `students` collection that removes any
identifiable information about each student:

```
db.createView(
  "anonStudents2025",
  "students",
  [{$match: {"graduationYear": 2025}}, {$project: {name: 0, email: 0}}]
);
```

We can now query this view for its contents without exposing the PII in the underlying collection.
We also don't need to manually filter for 2025 graduates or project out the identifiable fields,
since the view takes care of that for us.

```
// Get a record of all Computer Science students without PII.
db.anonStudents2025.find({"major": "Computer Science"});

// Get a list of majors that were achieved in 2025.
db.anonStudents2025.distinct("major");

// Get the maximum, minimum, and average GPA within each major.
db.anonStudents2025.aggregate(
    [{$group: { _id: "$major", maxGPA: {$max: "$GPA" }, minGPA: {$min: "$GPA" }, avgGPA: {$avg: "$GPA" }}}]);
```

Notice that even though the view is using an `aggregate()` for us under the hood, we can still
access its contents using `find()`, `distinct()`, or `count()`. In these cases, the command is
internally transformed into an `aggregate` command. See:

- [`runFindAsAgg()`](https://github.com/mongodb/mongo/blob/e16bc2248a3410167e39d09bb9bc29a96f026ead/src/mongo/db/commands/query_cmd/find_cmd.cpp#L998)
- [`runDistinctAsAgg()`](https://github.com/mongodb/mongo/blob/e16bc2248a3410167e39d09bb9bc29a96f026ead/src/mongo/db/commands/query_cmd/distinct.cpp#L753)
- [`runCountAsAgg()`](https://github.com/mongodb/mongo/blob/e16bc2248a3410167e39d09bb9bc29a96f026ead/src/mongo/db/commands/query_cmd/count_cmd.cpp#L612)

When an `aggregate` command targets a view,
[`_runAggregate()`](../commands/query_cmd/run_aggregate.cpp) acquires catalog state and discovers
the namespace is a view. It then resolves the view by creating a
[`ResolvedViewAggExState`](../commands/query_cmd/aggregation_execution_state.h), which retargets the
request at the underlying collection and stores the resolved view information. Locks on the view
namespace are released before proceeding. From here the path diverges based on sharding awareness:

- **Sharding-aware**: `runAggregateOnShardedView()` transitions into the router role to obtain
  routing information for the underlying collection. If the collection lives on this shard, it sets
  up the shard role and calls `executeResolvedAggregate()` locally. Otherwise, it throws a kick-back
  exception so mongos can re-execute the resolved pipeline across the appropriate shards.
- **Non-sharded**: The resolved state simply replaces the original `AggExState` and the function
  falls through to `executeResolvedAggregate()`.

### Applying the view pipeline

The view pipeline is not simply prepended in all cases. The actual application is driven by the
first stage's [`FirstStageViewApplicationPolicy`](../pipeline/lite_parsed_document_source.h), which
is consulted during [`LiteParsedPipeline::handleView()`](../pipeline/lite_parsed_pipeline.cpp):

- **`kDefaultPrepend`** (the default): The desugared view pipeline is cloned and stitched to the
  front of the user's pipeline. This is the standard path for most aggregations on views.
- **`kDoNothing`**: The view pipeline is _not_ prepended. The stage itself is responsible for
  incorporating the view information internally via its
  [`bindViewInfo()`](../pipeline/lite_parsed_document_source.h) override. This allows extension
  stages (e.g. search stages) to handle view resolution with custom logic.

Regardless of the policy, every stage in the pipeline has its `bindViewInfo()` called with the
[`ViewInfo`](../pipeline/lite_parsed_document_source.h) and the resolved namespace map, giving all
stages a chance to react to the view context (e.g. for secondary namespace resolution in `$lookup`
or `$unionWith`).

The joint pipeline (view definition + user query) will very likely have room for optimization (if
both the view definition and aggregate command contain a `$match` stage that may be combined, for
example); this is taken care of by [pipeline rewrites](../pipeline/README.md).

Continuing with the first query in the previous example:

```
db.anonStudents2025.find({"major": "Computer Science"});
```

is rewritten under the hood as:

```
db.students.aggregate([
  {$match: {graduationYear: {$eq: 2025.0}}},
  {$project: {name: 0, email: 0, _id: 1}},
  {$match: {major: {$eq: "Computer Science"}}}
]);
```

which is eventually optimized to:

```
db.students.aggregate([
  {$match: {$and: [{graduationYear: {$eq: 2025.0}}, {major: {$eq: "Computer Science"}}]}}
  {$project: {name: 0, email: 0, _id: 1}}
]);
```

---

[Return to Cover Page](../query/README_QO.md)
