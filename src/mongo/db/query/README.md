# Query System Internals

*Disclaimer*: This is a work in progress. It is not complete and we will
do our best to complete it in a timely manner.

## Overview

The query system generally is responsible for interpreting the user's
request, finding an optimal way to satisfy it, and to actually compute
the results. It is primarily exposed through the find and aggregate
commands, but also used in associated read commands like count,
distinct, and mapReduce.

Here we will divide it into the following phases and topics:

 * **Command Parsing & Validation:** Which arguments to the command are
   recognized and do they have the right types?
 * **Query Language Parsing & Validation:** More complex parsing of
   elements like query predicates and aggregation pipelines, which are
   skipped in the first section due to complexity of parsing rules.
 * **Query Optimization**
     * **Normalization and Rewrites:** Before we try to look at data
       access paths, we perform some simplification, normalization and
       "canonicalization" of the query.
     * **Index Tagging:** Figure out which indexes could potentially be
       helpful for which query predicates.
     * **Plan Enumeration:** Given the set of associated indexes and
       predicates, enumerate all possible combinations of assignments
       for the whole query tree and output a draft query plan for each.
     * **Plan Compilation:** For each of the draft query plans, finalize
       the details. Pick index bounds, add any necessary sorts, fetches,
       or projections
     * **Plan Selection:** Compete the candidate plans against each other
       and select the winner.
     * **Plan Caching:** Attempt to skip the expensive steps above by
       caching the previous winning solution.
 * **Query Execution:** Iterate the winning plan and return results to the
   client.

In this documentation we focus on the process for a single node or
replica set where all the data is expected to be found locally. We plan
to add documentation for the sharded case in the src/mongo/s/query/
directory later.

### Command Parsing & Validation

The following commands are generally maintained by the query team, with
the majority of our focus given to the first two.
* find
* aggregate
* count
* distinct
* mapReduce
* update
* delete
* findAndModify

The code path for each of these starts in a Command, named something
like MapReduceCommand or FindCmd. You can generally find these in
src/mongo/db/commands/.

The first round of parsing is to piece apart the command into its
components. Notably, we don't yet try to understand the meaning of some
of the more complex arguments which are more typically considered the
"MongoDB Query Language" or MQL. For example, these are find's 'filter',
'projection', and 'sort' arguments, or the individual stages in the
'pipeline' argument to aggregate.

Instead, command-level parsing just takes the incoming BSON object and
pieces it apart into a C++ struct with separate storage for each
argument, keeping the MQL elements as mostly unexamined BSON for now.
For example, going from one BSON object with `{filter: {$or: [{size: 5},
{size: 6}]}, skip: 4, limit: 5}` into a C++ object which stores `filter`,
`skip` and 'limit' as member variables. Note that `filter` is stored as
a BSONObj - we don't yet know that it has a `$or` inside. For this
process, we prefer using an Interface Definition Language (IDL) tool to
generate the parser and actually generate the C++ class itself.

### The Interface Definition Language

You can find some files ending with '.idl' as examples, a snippet may
look like this:

```
commands:
    count:
        description: "Parser for the 'count' command."
        command_name: count
        cpp_name: CountCommand
        strict: true
        namespace: concatenate_with_db_or_uuid
        fields:
            query:
                description: "A query that selects which documents to count in the collection or
                view."
                type: object
                default: BSONObj()
            limit:
                description: "The maximum number of matching documents to count."
                type: countLimit
                optional: true
```

This file (specified in a YAML format) is used to generate C++ code. Our
build system will run a python tool to parse this YAML and spit out C++
code which is then compiled and linked. This code is left in a file
ending with '\_gen.h' or '\_gen.cpp', for example
'count\_command\_gen.cpp'. You'll notice that things like whether it is
optional, the type of the field, and any defaults are included here, so
we don't have to write any code to handle that.

The generated file will have methods to get and set all the members, and
will return a boost::optional for optional fields. In the example above,
it will generate a CountCommand::getQuery() method, among others.

### Other actions performed during this stage

As stated before, the MQL elements are unparsed - the query here is
still an "object", stored in BSON without any scrutiny at this point.

This is how we begin to transition into the next phase where we piece
apart the MQL. Before we do that, there are a number of important things
that happen on these structures.

#### Various initializations and setup

Pretty early on we will set up context on the "OperationContext" such as
the request's read concern, read preference, maxTimeMs, etc. The
OperationContext is generally accessible throughout the codebase, and
serves as a place to hang these operation-specific settings.

Also early in the command implementation we may also take any relevant
locks for the operation. We usually use some helper like
"AutoGetCollectionForReadCommand" which will do a bit more than just
take the lock. For example, it will also ensure things are set up
properly for our read concern semantics and will set some debug and
diagnostic info which will show up in one or all of '$currentOp', the
logs and system.profile collection, the output of the 'top' command,
'$collStats', and possibly some others.

Once we have obtained a lock we can safely get access to the
collection's default collation, which will help us construct an
ExpressionContext.

An "ExpressionContext" can be thought of as the query system's version
of the OperationContext. Please try your hardest to ignore the name, it
is a legacy name and not particularly helpful or descriptive. Once upon
a time it was used just for parsing expressions, but it has since
broadened scope and perhaps "QueryContext" or something like that would
be a better name. This object stores state that may be useful to access
throughout the lifespan of a query, but is probably not relevant to any
other operations. This includes things like the collation, a time zone
database, and various random booleans and state. It is expected to make
an ExpressionContext before parsing the query language aspects of the
request. The most obvious reason this is required is that the
ExpressionContext holds parsing state like the variable resolution
tracking and the maximum sub-pipeline depth reached so far.

#### Authorization checking

In many but not all cases, we have now parsed enough to check whether
the user is allowed to perform this request. We usually only need the
type of command and the namespace to do this. In the case of mapReduce,
we also take into account whether the command will perform writes based
on the output format.

A more notable exception is the aggregate command, where different
stages can read different types of data which require special
permissions. For example a pipeline with a $lookup or a $currentOp may
require additional privileges beyond just the namespace given to the
command. We defer this authorization checking until we have parsed
further to a point where we understand which stages are involved. This
is actually a special case, and we use a class called the
`LiteParsedPipeline` for this and other similar purposes.

 The `LiteParsedPipeline` class is constructed via a semi-parse which
only goes so far as to tease apart which stages are involved. It is a
very simple model of an aggregation pipeline, and is supposed to be
cheaper to construct than doing a full parse. As a general rule of
thumb, we try to keep expensive things from happening until after we've
verified the user has the required privileges to do those things. 

This simple model can be used for requests we want to inspect before
proceeding and building a full model of the user's query or request. As
some examples of what is deferred, this model has not yet verified that
the input is well formed, and has not yet parsed the expressions or
detailed arguments to the stages. You can check out the
`LiteParsedPipeline` API to see what kinds of questions we can answer
with just the stage names and pipeline structure.

#### Additional Validation

In most cases the IDL will take care of all the validation we need at
this point. There are some constraints that are awkward or impossible to
express via the IDL though. For example, it is invalid to specify both
`remove: true` and `new: true` to the findAndModify command. This would
be requesting the post-image of a delete, which is nothing.

#### Non-materialized view resolution

We have a feature called 'non-materialized read only views' which allows
the user to store a 'view' in the database that mostly presents itself
as a read-only collection, but is in fact just a different view of data
in another collection. Take a look at our documentation for some
examples. Before we get too far along the command execution, we check if
the targeted namespace is in fact a view. If it is, we need to re-target
the query to the "backing" collection and add any view pipeline to the
predicate. In some cases this means a find command will switch over and
run as an aggregate command, since views are defined in terms of
aggregation pipelines.

## Query Language Parsing & Validation

TODO from here on.
