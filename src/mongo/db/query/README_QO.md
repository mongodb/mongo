# Query Optimization Architecture Guide

This page provides an overview of the source code architecture for MongoDB's Query Optimization system. It is designed for engineers working on the core server, with introductory sections offering low-level details particularly useful for new members of the QO team.

## Table of Contents

1. [Parsing](../commands/query_cmd/README.md)
   - [Stable API](../STABLE_API_README.md)
1. [Logical Models](README_logical_models.md)
   - Heuristic Rewrites
     - [`MatchExpression` Rewrites](../matcher/README.md)
     - [Pipeline Rewrites](../pipeline/README.md)
   - [Views](../views/README.md)
1. Query Planning
   - [Plan Enumeration](plan_enumerator/README.md)
   - [Classic Runtime Planning](../exec/runtime_planners/classic_runtime_planner/README.md)
1. [Explain](README_explain.md)
1. [Plan Cache](plan_cache/README.md)
1. [Cluster Planning](../../s/query/planner/README.md)
1. Testing
   - [Golden Testing](../../../../docs/golden_data_test_framework.md)
   - [QueryTester](query_tester/README.md)
   - [Fuzzers](https://github.com/10gen/jstestfuzz/blob/master/HitchhikersGuide.md)

## High-Level Diagram

```mermaid
---
config:
  themeVariables:
    fontSize: 32px
---
flowchart TD
 subgraph Client[" "]
        mr["mapReduce"]
        agg["aggregate"]
        find["find"]
        count["count"]
        dist["distinct"]
        del["delete"]
        update["update"]
        fam["findAndModify"]
  end
 subgraph s1[" "]
        D1["DocumentSourceExecution"]
        D2["PlanExecutor"]
        D3["Storage API"]
  end
    find --> cq["CanonicalQuery"]
    count --> cq
    dist --> cq
    cq -- filter --> me["MatchExpression"]
    del -- filter --> cq
    update -- filter --> cq
    fam -- filter --> cq
    me --> C1["optimizeMatchExpression()"]
    C1 --> C3["Plan Enumerator"]
    C3 --> n2["Candidate QuerySolutions"]
    n2 --> C4["Classic Multiplanner"] & C5["Classic Multiplanner for SBE"] & C6["Cost Based Ranker"]
    C7["Cardinality Estimation"] --o C6
    C8["Cost Model"] --o C6
    C4 --> C12["Winning QuerySolution"]
    C5 --> C12
    C6 --> C12
    C11{"Can pushdown to find?"} -- Yes --> cq
    C12 --> D2
    C11 -- No --> D1
    D1 --> D2
    D2 --> D3
    n1["Pipeline::optimize()"] --> C11
    agg --> B1["Pipeline"]
    mr -- "Deprecated in v5.0 in favor of agg" --> B1
    cq -- projection<br>sort --> C3
    B1 --> n1
    D1@{ shape: subproc}
    D2@{ shape: lin-rect}
    D3@{ shape: cyl}
    cq@{ shape: dbl-circ}
    me@{ shape: dbl-circ}
    C1@{ shape: subproc}
    C3@{ shape: subproc}
    n2@{ shape: docs}
    C4@{ shape: subproc}
    C5@{ shape: subproc}
    C6@{ shape: subproc}
    C7@{ shape: lean-r}
    C8@{ shape: lean-r}
    C12@{ shape: doc}
    n1@{ shape: subproc}
    B1@{ shape: dbl-circ}
```
