# Query Optimization Architecture Guide

This page provides an overview of the source code architecture for MongoDB's Query Optimization system. It is designed for engineers working on the core server, with introductory sections offering low-level details particularly useful for new members of the QO team.

## Table of Contents

- [I. Parsing](#parsing)
- [II. Logical Models](#logical-models)
  - [Views](#views)
  - [Heuristic Rewrites](#heuristic-rewrites)
- [III. Index Selection](#iv-index-selection)
  - [QueryPlanner and Plan Enumeration](#queryplanner-and-plan-enumeration)
  - [Classic Runtime Planners](#classic-runtime-planners)
  - [Cost Based Ranker](#cost-based-ranker)
    - [Cost Model](#cost-model)
    - [Cardinality Estimation](#cardinality-estimation)
- [IV. Explain](#explain)
- [V. Plan Cache](#plan-cache)
- [VI. Cluster Planning](#cluster-planning)
- [VII. Testing](#testing)
  - [Golden Testing](#golden-testing)
  - [QueryTester](./query_tester/README.md)
  - [Fuzzers](#fuzzers)

## Glossary: QO Terminology and Acronyms

- **Aggregation**: The subsystem that runs aggregate stages.
- **BSON**: Binary-encoded serialization of JSON-like documents.
  - A data format developed by MongoDB for data representation in its core.
- **CanonicalQuery**: A standardized form for queries, in BSON.
  - It works as a container for the parsed query, projection, and sort portions of the original query message. The filter portion is parsed into a **MatchExpression**.
- **DocumentSource**: Represents one stage in an **aggregation** **pipeline**
  - Not necessarily one-to-one with the stages in the user-defined pipeline.
- **ExpressionContext**: An object that stores state that may be useful to access throughout the lifespan of a query, but is probably not relevant to any other operations. This includes the collation, a time zone database, various random booleans and state, etc.
- **Find**: The subsystem that runs **find** stages and **pushed-down** **aggregate** stages.
- **IDL**: Interface Definition Language. YAML-formatted files to generate C++ code.
- **LiteParsedPipeline**: A very simple model of an **aggregate** **pipeline**, constructed through a semi-parse that proceeds just enough to tease apart the stages that are involved.
  - It has neither verified that the input is well-formed, nor parsed the expressions or detailed arguments to the stages. It can be used for requests that we want to inspect before proceeding and building a full model of the user's query or request.
- **MatchExpression**: The parsed Abstract Syntax Tree (AST) from the filter portion of the query.
- **MQL**: MongoDB Query Language.
- **Plan Cache**: Stores previously generated query plans to allow for faster retrieval and execution of recurring queries by avoiding the need to generate and score possible query plans from scratch.
- **PlanExecutor**: An abstract type that executes a **QuerySolution** plan by cranking its tree of stages into execution. **PlanExecutor** has three primary subclasses:
  1. PlanExecutorImpl: Executes **find** stages
  1. PlanExecutorPipeline: Executes **aggregation** stages.
  1. PlanExecutorSBE: Executes SBE plans.
- **Pipeline**: A list of **DocumentSources** which handles a part of the optimization.
- **Pushdown**: Convert an **aggregate** stage in the **pipeline** to a **find** stage.
- **QuerySolution**: A tree structure of QuerySolutionNodes that represents one possible execution plan for a query.
  - Various operation nodes inherit from QuerySolutionNode
    - For example: CollectionScanNode, FetchNode, IndexScanNode, OrNode, etc.
  - Generally speaking, one winning **QuerySolution** is the output of the QO system.

## High-Level Diagram

```mermaid
graph TD
  %% Client Section
  subgraph Client
    A1[mapReduce]
    A2[aggregate]
    A3[find]
    A4[count]
    A5[distinct]
    A6[delete]
    A7[update]
    A8[findAndModify]
  end
  %% Parsing Section
  subgraph Parsing
    B1@{ shape: dbl-circ, label: Pipeline }
    B2@{ shape: subproc, label: "DocumentSource parsing" }
    B3@{ shape: dbl-circ, label: "CanonicalQuery" }
    B4[ParsedUpdate/ParsedDelete]
    B5@{ shape: dbl-circ, label: "MatchExpression" }
  end
  %% Optimization Section
  subgraph Optimization
    C1@{ shape: subproc, label: "MatchExpression::optimize()" }
    C2@{ shape: subproc, label: "Pipeline::optimize()" }
    C3@{ shape: lin-rect, label: "Plan Enumerator" }
    C4@{ shape: docs, label: "Classic Multiplanner" }
    C5@{ shape: docs, label: "Classic Multiplanner for SBE" }
    C6@{ shape: docs, label: "Cost Based Ranker" }
    C7@{ shape: lean-r, label: "Cardinality Estimation" }
    C8@{ shape: lean-r, label: "Cost Model" }
    C11{Can pushdown to find?}
    C12@{ shape: doc, label: "QuerySolution" }
  end
  %% Execution Section
  subgraph Execution
    D1@{ shape: subproc, label: "DocumentSourceExecution" }
    D2@{ shape: lin-rect, label: "PlanExecutor" }
    D3@{ shape: cyl, label: "Storage API" }
  end
  subgraph Legend
    E13@{ shape: "rectangle", label: "Command"}
    E14@{ shape: "dbl-circ", label: "Standard IR"}
    E15@{ shape: subproc, label: "Process" }
    E16@{ shape: docs, label: "Query Plans"}
    E20@{ shape: doc, label: "Query Plan"}
    E17@{ shape: cyl, label: "Database" }
    E18{Decision}
    E19@{ shape: lean-r, label: "Input" }
  end
  %% Client to Parsing Links
  A1 -- Deprecated in 5.0 in favor of agg --> B1
  A2 --> B1
  A3 --> B3
  A4 --> B3
  A5 --> B3
  A6 --> B4
  A7 --> B4
  A8 --> B4
  %% Parsing Links
  B1 --> B2
  B4 --> B3
  B3 -- Build from filter --> B5
  %% Parsing to Optimization Links
  B2 --> C2
  B5 --> C1
  %% Optimization Links
  C1 --> C3
  C3 --> C4
  C3 --> C5
  C3 --> C6
  C7 --o C6
  C8 --o C6
  C4 --> C12
  C5 --> C12
  C6 --> C12
  C2 --> C11
  %% Optimization to Parsing Links
  C11 -.-> |Yes| B3
  %% Optimization to Execution Links
  C12 --> D2
  C11 --> |No| D1
  %% Execution Links
  D1 --> D2
  D2 --> D3
  %% Invisible Links for Formatting
  B3 ~~~~ Execution
  Execution ~~~~ Legend
  D3 ~~~~ E13
```
