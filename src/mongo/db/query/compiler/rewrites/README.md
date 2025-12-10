# Rule-based Rewrite Engine

## Overview

The rule-based rewrite engine is a simple but generic-purpose engine for applying sets of rewrite rules to a data structure. It is currently only used for [optimizing aggregation pipelines](https://github.com/mongodb/mongo/blob/e4bf22b6936f3795e11890c908521825120c8a05/src/mongo/db/pipeline/README.md). The following sections describe different components that make up the engine ([the rules](#rules), [the rewrite context](#rewrite-context), and [the engine](#rewrite-engine) itself).

## Rules

The rewrite engine executes rules, which are defined by a name, precondition and transform functions, a priority and a set of tags. The precondition function determines whether the transform function should run. Priority is used to determine the order in which rules are applied when multiple rules may apply to the same element. The tags allow the engine to be invoked to only apply a certain subset of rules.
https://github.com/mongodb/mongo/blob/d8c7211ff2b04e961019b3939500221b94149931/src/mongo/db/query/compiler/rewrites/rule_based_rewriter.h#L51-L81

## Rewrite Engine

The engine is a [generic class](https://github.com/mongodb/mongo/blob/0e6163a2018345a86baf5bd4bff03cefd224daec/src/mongo/db/query/compiler/rewrites/rule_based_rewriter.h#L164-L165) responsible for driving the rewrite process and maintaining a priority queue of rules that are applicable to the element that is being rewritten. It can be specialized to work with any data structure by providing it with an implementation of the [rewrite context](#rewrite-context) that knows how to walk and modify that structure. The engine is invoked by calling the [`applyRules()`](https://github.com/mongodb/mongo/blob/0e6163a2018345a86baf5bd4bff03cefd224daec/src/mongo/db/query/compiler/rewrites/rule_based_rewriter.h#L181) method (see [`optimize.cpp`](https://github.com/mongodb/mongo/blob/126ab84794ef530fd2503453c9f8828743a4e7e7/src/mongo/db/pipeline/optimization/optimize.cpp#L44-L48) for example usage). The rewrite process is essentially a loop that asks the rewrite context for all rules that can apply to the current element, attempts them in priority order, and either advances to the next element or retries the rules on the same element depending on whether any transform reported that it changed the position of the current element.
https://github.com/mongodb/mongo/blob/126ab84794ef530fd2503453c9f8828743a4e7e7/src/mongo/db/query/compiler/rewrites/rule_based_rewriter.h#L181-L203

Besides constructing the engine and calling [`applyRules()`](https://github.com/mongodb/mongo/blob/0e6163a2018345a86baf5bd4bff03cefd224daec/src/mongo/db/query/compiler/rewrites/rule_based_rewriter.h#L181), users of the engine should not interact with it directly. Rules never interact with the engine directly either.

## Rewrite Context

The rewrite engine itself is agnostic to the details of the data structure that it is rewriting. It relies on the interface provided by a concrete [`RewriteContext`](https://github.com/mongodb/mongo/blob/0e6163a2018345a86baf5bd4bff03cefd224daec/src/mongo/db/query/compiler/rewrites/rule_based_rewriter.h#L90) implementation to walk and modify the structure, and to decide which rules can apply to which elements. The interface is defined as follows: https://github.com/mongodb/mongo/blob/0e6163a2018345a86baf5bd4bff03cefd224daec/src/mongo/db/query/compiler/rewrites/rule_based_rewriter.h#L92-L112

Similarly, rules have access to the context and can use it to enqueue additional rules. The context can also expose additional helpers to rules, e.g. for modifying the structure that is being rewritten. See [`rule_based_rewrites::pipeline::Transforms`](https://github.com/mongodb/mongo/blob/0e6163a2018345a86baf5bd4bff03cefd224daec/src/mongo/db/pipeline/optimization/rule_based_rewriter.h#L202) for an example.
