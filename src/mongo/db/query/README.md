# Query System Internals

## Overview

The query system is responsible for interpreting the user's request, finding an optimal way to
satisfy it, and to actually compute the results. It is primarily exposed through the `find` and
`aggregate` commands, but also used in associated read commands like `count`, `distinct`, and
`mapReduce`.

> This new README is split off into many different PRs. Please refer to the [old README][old readme]
> for any information that is yet missing.

## Architecture

- Command parsing & validation
- Query Language parsing & validation
- Optimizer
- Execution engine
- Sharding
- [Views][views]

## Features

- Change Streams
- Field-level encryption
- Geo
- [Query shape][query shape]
- [Query stats][query stats]
- Queryable encryption
- Search
- [Timeseries][timeseries]
- [Vector search][vector search]

## Glossary

- <a id="glossary-Aggregation"></a>Aggregation: The subsystem that runs `aggregate` stages.
- <a id="glossary-BSON"></a>[BSON][bson]: Bin­ary-en­coded serialization of JSON-like documents. A
  data format developed by MongoDB for data representation in its core.
- <a id="glossary-CanonicalQuery"></a>[Canonical query][canonicalquery]: A standardized form for
  queries, in BSON. It works as a container for the parsed query and projection portions of the
  original query message. The filter portion is parsed into a
  [`MatchExpression`](#glossary-MatchExpression).
- <a id="glossary-DocumentSource"></a>DocumentSource: Represents one stage in an
  [`aggregation`](#glossary-Aggregation) [pipeline](#glossary-Pipeline); not necessarily
  one-to-one with the stages in the user-defined pipeline.
- <a id="glossary-ExpressionContext"></a>[ExpressionContext][expressioncontext]: An object that
  stores state that may be useful to access throughout the lifespan of a query, but is probably
  not relevant to any other operations. This includes the collation, a time zone database, various
  random booleans and state, etc.
- <a id="glossary-Find"></a>Find: The subsystem that runs [`find`](#glossary-Find) stages and
  [pushed-down](#glossary-Pushdown) [`aggregate`](#glossary-Aggregation) stages.
- <a id="glossary-IDL"></a>[IDL][idl]: Interface Definition Language. YAML-formatted files to
  generate C++ code.
- <a id="glossary-LiteParsedPipeline"></a>[LiteParsedPipeline][liteparsedpipeline]: A very simple
  model of an [`aggregate`](#glossary-Aggregation) [pipeline](#glossary-Pipeline). It is
  constructed through a semi-parse that proceeds just enough to tease apart the stages that are
  involved. It has neither verified that the input is well-formed, nor parsed the expressions or
  detailed arguments to the stages. It can be used for requests that we want to inspect before
  proceeding and building a full model of the user's query or request.
- <a id="glossary-MatchExpression"></a>MatchExpression: The parsed Abstract Syntax Tree (AST) from
  the filter portion of the query.
- <a id="glossary-MQL"></a>MQL: MongoDB Query Language.
- <a id="glossary-Pipeline"></a>Pipeline: A list of [`DocumentSources`](#glossary-DocumentSource)
  which handles a part of the optimization.
- <a id="glossary-Pushdown"></a>Pushdown: Convert an [`aggregate`](#glossary-Aggregation)
  stage in the [pipeline](#glossary-Pipeline) to a [`find`](#glossary-Find) stage.

<!-- Links -->

[old readme]: README_old.md
[timeseries]: timeseries/README.md
[query stats]: query_stats/README.md
[query shape]: query_shape/README.md
[vector search]: ../pipeline/search/README.md
[bson]: https://bsonspec.org/
[idl]: ../../idl/README.md
[canonicalquery]: canonical_query.h
[liteparsedpipeline]: ../pipeline/lite_parsed_pipeline.h
[expressioncontext]: ../pipeline/expression_context.h
[views]: README_views.md
