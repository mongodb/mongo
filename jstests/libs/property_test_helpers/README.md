# Core Property-Based Tests

For a short introduction to property-based testing or fast-check, see [Appendix](#appendix).

## Core PBT Design

The 'Core PBTs' are a subset of our property-based tests that use a shared schema and models. Their purpose is to provide basic coverage of our query language that may not be tested by the rest of our jstests. This means only simple stages such as $project, $match, $sort, etc are covered. More complicated stages such as $lookup or $facet are not tested. PBTs outside of the core set may test these more complex features.

These tests have been highly effective at finding bugs. As of writing they have caught 24 bugs in 8 months. See [SERVER-89308](https://jira.mongodb.org/browse/SERVER-89308) for a full list of issues.

The Core PBT design is built off of a few key principles about randomized testing:

### Properties Dictate the Models

In our fuzzer, we have grammar for most of MQL. While this provides more coverage, it means the property we assert is weaker. We can add as much as we'd like to the model, because the property comes second to the model. We're willing to add exceptions to the property to make it work.

However, the "model dictates the property" design also backfired, because in addition to exceptions in the property, we need to post-process the generated queries. Adding $sort to several places throughout an aggregation pipeline means we are no longer testing MQL, but rather an artificial subset of MQL that a user would never write.

For this reason, the properties come first in our Core PBTs, and have few exceptions. They dictate what model we use so no postprocessing is needed. The PBT models are significantly smaller than the fuzzer models.

### Small Schema

#### Number of Fields

A small number of fields in our schema allows us to find interesting interactions more easily.

An example of an interaction could be query optimizations. Let's say an optimization on `[{$match: {*field*: 5}}, {$sort: {*field*: 1}}]` only kicks in when the two fields are the same. In a PBT where there are one thousand possible fields (`a`, `b`, `c`, but also `a.b.c`, `a.a.a` and all combinations), the probability of finding this optimization is `1/1000`. With six fields, it's increased to `1/6`.

Another interaction is between queries and indexes. Queries and indexes generated from a small schema make the indexes more likely to be used.

Bugs tend to come from interactions and special cases. A query that has no optimizations applied and does not use an index requires much less complicated logic, which is correlated to less bugs.

#### Simple Values to Avoid MQL Inconsistencies

Related to [Properties Dictate the Models](#properties-dictate-the-models), a simpler document model also allows for stronger properties.

There are inconsistencies in our query language that are accepted behavior, but cause issues in property-based testing. We can work around them by being careful about the values we allow in documents.

[SERVER-12869](https://jira.mongodb.org/browse/SERVER-12869) is an issue that stems from null and missing being encoded the same way in our index format. This means a covering plan (a plan with no `FETCH` node) cannot distinguish between null and missing. This inconsistency is the cause of lots of noise from our fuzzer, since one differing value in a query result can propogate. In our Core PBTs, we do not allow missing fields. This means:

- Documents must have all fields in the schema
- We can only index fields in the schema
- Queries can only reference fields in the schema

`null` is allowed.

Floating point values are another area the PBTs avoid. Results can differ depending on the order of floating point operations. These differences can propogate. For this reason the only number values allowed are integers.

## Modeling Workloads

A workload consists of a collection model and an aggregation model, in the following format:

```
{
   collSpec: {
       isTS:      true/false to indicate if the collection should be time-series
       docs:      a list of documents
       indexes:   a list of indexes
   },
   queries:  a list of aggregation pipelines
}
```

Using one workload model instead of separate (and independent) collection models and agg models allows them to be interrelated.
For example, if we want to model a PBT to test partial indexes where every query should satisfy the partial index filter, we can write:

```
fc.record({
    partialFilter: partialFilterPredicateModel,
    docs: docsModel,
    indexes: indexesModel,
    aggs: aggsModel
}).map(({partialFilter, docs, indexes, aggs}) => {
    // Append {partialFilterExpression: partialFilter} to all index options
    // Prefix every query with {$match: partialFilter}
    // Return our workload object.
});
```

and this is a valid workload model. If the collection and aggregation models are passed separately, they would be independent an unable to coordinate with shared arbitraries (like `partialFilter`).

### Schema

The Core PBT schema is:

```
{
    _id:      a unique integer
    t:        a date value
    m:        an object with subfields 'm1' and 'm2'. both are simple scalars
    array:    an array of scalars, other arrays, or objects. this is the only field that is allowed to be an array.
    a:        any simple scalar: integer, boolean, string, date, null
    b:        same as `a`
}
```

For now, this is also a valid model for a document in a time-series collection (where `t` is the time field and `m` is the meta field), but the models may diverge.

### Query Generation

TODO SERVER-102567 expand readme

#### Query Families

Rather than generating single, standalone queries, our query model generates a "family" of queries.
At its leaves, a query family contains multiple values that the leaf could take on. For example instead of generating a single query with a concrete value `1` at the leaf:

```
[{$match: {a: 1}}, {$project: {b: 0}}]
```

We generate `1,2,3` as potential values this slot can hold.

```
[{$match: {a: {concreteValues: [1,2,3]}}}, {$project: {b: 0}}]
```

Then we extract several queries that have the same shape.

```
[{$match: {a: 1}}, {$project: {b: 0}}]
[{$match: {a: 2}}, {$project: {b: 0}}]
[{$match: {a: 3}}, {$project: {b: 0}}]
```

This allows us to write properties that use the plan cache more often rather than relying on chance.
Properties can use the `getQuery` interface to ask for queries with different shapes, or the same shape with different leaf values plugged in.

## List of Core PBTs

[index_correctness_pbt.js](../../core/query/index_correctness_pbt.js)

[cache_correctness_pbt.js](../../core/query/plan_cache/cache_correctness_pbt.js)

[run_all_plans_pbt.js](../../core/query/run_all_plans_pbt.js)

[cache_usage_pbt.js](../../core/query/plan_cache/cache_usage_pbt.js)

[queries_create_one_cache_entry_pbt.js](../../core/query/plan_cache/queries_create_one_cache_entry_pbt.js)

[agg_stages_basic_behavior_pbt.js](../../aggregation/sources/agg_stages_basic_behavior_pbt.js)

Details are provided at the top of each file.

# Appendix

## Property-Based Testing (PBT)

Property-based testing is a testing method that asserts properties hold over many example inputs. In our use of PBT, it involves two components, a "model" and a "property function". The model is a description of the object we are testing. It is used to generate examples of what the object looks like. These examples are routed into the property function, which asserts that the object has the characteristics we expect them to have.

Let's say we wrote a new integer addition function `add` that we'd like to test. We could calculate the correct answer to different addition problems, and assert that `add` behaves correctly.

```
assert.eq(add(1, 2), 3);
assert.eq(add(-1, 1), 0);
...
```

In addition to tests written with concrete values, we could also write a PBT to test for characteristics we expect `add` to have. Addition is commutative for example, meaning `add(a, b)` should always equal `add(b, a)`. We can write a function for this:

```
function testAdd(a, b){
    assert.eq(add(a, b), add(b, a));
}
```

The input to `testAdd` could use the builtin Javascript `Random` package, or a PBT library such as fast-check.

The way the query team uses PBT tends to be more complex, and almost always involves modeling a subset of our query language, documents, and indexes. Our fuzzer is a form of property-based testing, since we generate random queries and assert correctness against different controls (an older mongo version, a collection without indexes, etc)

## fast-check

fast-check (located in jstests/third_party/fast_check/fc-3.1.0.js) is a property-based testing framework for javascript/typescript. It provides building-block components to use for larger models, and has functionality to test properties against these models. It also has built-in logic for shrinking (minimizing) counterexamples to properties.

For an example of how to use fast-check to write a property-based test, see [project_coalescing.js](../../aggregation/sources/project/project_coalescing.js)
