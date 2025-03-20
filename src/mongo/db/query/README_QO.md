# Query Optimization Architecture Guide

This page provides an overview of the source code architecture for MongoDB's Query Optimization system. It is designed for engineers working on the core server, with introductory sections offering low-level details particularly useful for new members of the QO team.

_Disclaimer_: This guide (SPM-2301) is a work in progress.

## Table of Contents

1. [Parsing](../commands/query_cmd/README.md)
   - [Stable API](../STABLE_API_README.md)
1. [Logical Models](README_logical_models.md)
   - Heuristic Rewrites
     - [`MatchExpression` Rewrites](../matcher/README.md)
     - [Pipeline Rewrites](../pipeline/README.md)
   - [Views](../views/README.md)
1. Index Selection
   - [QueryPlanner and Plan Enumeration](#)
   - [Classic Runtime Planning](classic_runtime_planner/README.md)
1. [Explain](README_explain.md)
1. [Plan Cache](plan_cache/README.md)
1. [Cluster Planning](../../s/query/planner/README.md)
1. Testing
   - [Golden Testing](../../../../docs/golden_data_test_framework.md)
   - [QueryTester](query_tester/README.md)
   - [Fuzzers](https://github.com/10gen/jstestfuzz/blob/master/HitchhikersGuide.md)

## Glossary: QO Terminology and Acronyms

- **Aggregation**: The subsystem that runs aggregate stages.
- **BSON**: Binary-encoded serialization of JSON-like documents.
  - A data format developed by MongoDB for data representation in its core.
- **`CanonicalQuery`**: A standardized form for queries, in BSON.
  - It works as a container for the parsed query, projection, and sort portions of the original query message. The filter portion is parsed into a **`MatchExpression`**.
- **`DocumentSource`**: Represents one stage in an **aggregation** **pipeline**
  - Not necessarily one-to-one with the stages in the user-defined pipeline.
- **`ExpressionContext`**: An object that stores state that may be useful to access throughout the lifespan of a query, but is probably not relevant to any other operations. This includes the collation, a time zone database, various random booleans and state, etc.
- **Find**: The subsystem that runs **find** stages and **pushed-down** **aggregate** stages.
- **IDL**: Interface Definition Language. YAML-formatted files to generate C++ code.
- **`LiteParsedPipeline`**: A very simple model of an **aggregate** **pipeline**, constructed through a semi-parse that proceeds just enough to tease apart the stages that are involved.
  - It has neither verified that the input is well-formed, nor parsed the expressions or detailed arguments to the stages. It can be used for requests that we want to inspect before proceeding and building a full model of the user's query or request.
- **`MatchExpression`**: The parsed Abstract Syntax Tree (AST) from the filter portion of the query.
- **MQL**: MongoDB Query Language.
- **Plan Cache**: Stores previously generated query plans to allow for faster retrieval and execution of recurring queries by avoiding the need to generate and score possible query plans from scratch.
- **`PlanExecutor`**: An abstract type that executes a **`QuerySolution`** plan by cranking its tree of stages into execution. **`PlanExecutor`** has three primary subclasses:
  1. `PlanExecutorImpl`: Executes **find** stages
  1. `PlanExecutorPipeline`: Executes **aggregation** stages.
  1. `PlanExecutorSBE`: Executes SBE plans.
- **Pipeline**: A list of **`DocumentSource`s** which handles a part of the optimization.
- **Pushdown**: Convert an **aggregate** stage in the **pipeline** to a **find** stage.
- **`QuerySolution`**: A tree structure of `QuerySolutionNode`s that represents one possible execution plan for a query.
  - Various operation nodes inherit from `QuerySolutionNode`
    - For example: `CollectionScanNode`, `FetchNode`, `IndexScanNode`, `OrNode`, etc.
  - Generally speaking, one winning **`QuerySolution`** is the output of the QO system.

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
 subgraph s1["Query Execution"]
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
    me --> C1["MatchExpression::optimize()"]
    C1 --> C3["Plan Enumerator"]
    C3 --> C4["Classic Multiplanner"] & C5["Classic Multiplanner for SBE"] & C6["Cost Based Ranker"]
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
    cq -- "projection<br>sort" --> C3
    B1 --> n1
    D1@{ shape: subproc}
    D2@{ shape: lin-rect}
    D3@{ shape: cyl}
    cq@{ shape: dbl-circ}
    me@{ shape: dbl-circ}
    B1@{ shape: dbl-circ}
    C1@{ shape: subproc}
    C3@{ shape: subproc}
    C4@{ shape: procs}
    C5@{ shape: procs}
    C6@{ shape: procs}
    C7@{ shape: lean-r}
    C8@{ shape: lean-r}
    C12@{ shape: dbl-circ}
    n1@{ shape: subproc}
```
