# Introduction

Content to be added by [SERVER-98882](https://jira.mongodb.org/browse/SERVER-98882), and should include:

- Mention where to find the QO arch guide.
- How to use this document.

# The big picture

Content to be added by [SERVER-98883](https://jira.mongodb.org/browse/SERVER-98883), and should include:

- Our three execution engines
  - Introduce SBE and link to [sbe.md](sbe.md)
  - Introduce Classic Engine and link to [classic_execution_engine.md](classic_execution_engine.md)
  - Introduce DocumentSources and link to [document_source_execution.md](document_source_execution.md)
- PlanExecutor
- Diagrams of hybrid execution
- Execution in a sharded context

# Shared Concepts

## MatchExpression, Expression, ExpressionContext

Content to be added by [SERVER-98884](https://jira.mongodb.org/browse/SERVER-98884)

## Yielding

Content to be added by [SERVER-99681](https://jira.mongodb.org/browse/SERVER-99681)

## CursorManager, getMores, and cursor lifetime

Content to be added by [SERVER-99682](https://jira.mongodb.org/browse/SERVER-99682)

# QE-specific sharding concepts

Content to be added by [SERVER-98885](https://jira.mongodb.org/browse/SERVER-98885), and should include:

- Shard filtering
- AsyncResultsMerger

# Change Streams

Content to be added by [SERVER-82744](https://jira.mongodb.org/browse/SERVER-82744)

# Index Key Generation

Content to be added by [SERVER-98886](https://jira.mongodb.org/browse/SERVER-98886)

# CRUD Execution Path on Mongos

Content to be added by [SERVER-99686](https://jira.mongodb.org/browse/SERVER-99686)

# Document Validation and JSON Schema

Content to be added by [SERVER-99687](https://jira.mongodb.org/browse/SERVER-99687)

# Collation

Content to be added by [SERVER-99685](https://jira.mongodb.org/browse/SERVER-99685)

# Exchange-based Parallelism

Content to be added by [SERVER-99688](https://jira.mongodb.org/browse/SERVER-99688), and should cover
the implementation in DocumentSources and/or the experimental implementation in SBE

# Explain

Content to be added by [SERVER-99689](https://jira.mongodb.org/browse/SERVER-99689)
