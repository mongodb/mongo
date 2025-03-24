# Query System Internals

## Overview

The query system is responsible for interpreting the user's request, finding an optimal way to satisfy it, and computing the final results. It is primarily exposed through the `find` and `aggregate` commands, but also used in associated read commands such as `count`, `distinct`, and `mapReduce` and write commands such as `update`, `delete`, and `findAndModify`.

## [Query Optimization Architecture Guide](README_QO.md)

The [QO Architecture Guide](README_QO.md) provides an overview of the query system components maintained by the QO team, including parsing, heuristic rewrites, query planning, plan caching, and query testing infrastructure.

## Additional Query Features

- Change Streams
- Field-Level Encryption
- Geo
- [Query shape](query_shape/README.md)
- [Query stats](query_stats/README.md)
- Queryable Encryption
- [Search](search/README.md)
- [Timeseries](timeseries/README.md)

## Glossary

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
  - Generally speaking, one winning **`QuerySolution`** is the output of the QO system and input of the QE system.
