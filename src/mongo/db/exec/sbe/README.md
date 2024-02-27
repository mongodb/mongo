# Slot Based Execution

SBE is a query execution engine implemented and maintained by the query execution team. It is one of
two engines maintained by the query execution team, the other being the classic execution engine. At
the time of writing, most queries currently use SBE and we intend to have SBE gradually replace the
classic engine over time.

SBE is a value based execution engine (unlike the classic engine, which is a document based one). It
is designed to be general enough that it can be used as the backend for executing any query
language. SBE models query execution as a tree of [execution stages](#plan-stages) which process
[values](#values) using [slots](#slots) and [expressions](#eexpressions). These concepts are
described in greater detail below.

## Values

A value is any entity that we are interested in accessing or manipulating during query execution.
These can range from simple values like integers and strings to more complex values like objects and
arrays. They closely resemble values in functional programming languages, that is, they are neither
shared, nor do they have identity (i.e. variables with the same numeric value are not conceptually
different entities). Some SBE values are [modeled off of
BSONTypes](https://github.com/mongodb/mongo/blob/f2b093acd48aee3c63d1a0e80a101eeb9925834a/src/mongo/bson/bsontypes.h#L63-L114)
while others represent internal C++ types such as
[collators](https://github.com/mongodb/mongo/blob/d19ea3f3ff51925e3b45c593217f8901373e4336/src/mongo/db/exec/sbe/values/value.h#L216-L217).

One type that deserves a special mention is `Nothing`, which indicates the absence of a value. It is
often used in SBE to indicate that a result cannot be computed instead of raising an exception
(similar to the [Maybe
Monad](<https://en.wikipedia.org/wiki/Monad_(functional_programming)#An_example:_Maybe>) in many
functional programming languages).

Values are identified by a [1 byte
TypeTag](https://github.com/mongodb/mongo/blob/d19ea3f3ff51925e3b45c593217f8901373e4336/src/mongo/db/exec/sbe/values/value.h#L132-L254)
, which denotes the type of the value that we are looking at, and an [8 byte
value](https://github.com/mongodb/mongo/blob/d19ea3f3ff51925e3b45c593217f8901373e4336/src/mongo/db/exec/sbe/values/value.h#L328-L331),
which is the value itself. If the value is shallow (that is, it requires 8 bytes or less to
represent), then the 8 bytes are used to store the value itself. If the value requires more than 8
bytes, the 8 bytes are used to store a pointer to a heap-allocated block of memory which contains
the value.

## EExpressions

In order to use values to implement the semantics of a given query language, we need a mechanism to
compute over them. To accomplish this, SBE provides an expression language defined by the
[EExpression
class](https://github.com/mongodb/mongo/blob/06a931ffadd7ce62c32288d03e5a38933bd522d3/src/mongo/db/exec/sbe/expressions/expression.h#L55-L66).
EExpressions form a tree and their goal is to produce values during evaluation. It's worth noting
that EExpressions aren't tied to expressions in the Mongo Query Language, rather, they are meant to
be building blocks that can be combined to express arbitrary query language semantics. Below is an
overview of the different EExpression types:

-   [EConstant](https://github.com/mongodb/mongo/blob/06a931ffadd7ce62c32288d03e5a38933bd522d3/src/mongo/db/exec/sbe/expressions/expression.h#L251-L279):
    As the name suggests, this expression type stores a single, immutable SBE value. An `EConstant`
    manages the value's lifetime (that is, it releases the value's memory on destruction if
    necessary).
-   [EPrimUnary and
    EPrimBinary](https://github.com/mongodb/mongo/blob/06a931ffadd7ce62c32288d03e5a38933bd522d3/src/mongo/db/exec/sbe/expressions/expression.h#L324-L414):
    These expressions represent basic logical, arithmetic, and comparison operations that take one and
    two arguments, respectively.
-   [EIf](https://github.com/mongodb/mongo/blob/06a931ffadd7ce62c32288d03e5a38933bd522d3/src/mongo/db/exec/sbe/expressions/expression.h#L440-L461):
    Represents an 'if then else' expression.
-   [EFunction](https://github.com/mongodb/mongo/blob/06a931ffadd7ce62c32288d03e5a38933bd522d3/src/mongo/db/exec/sbe/expressions/expression.h#L416-L438):
    Represents a named, built-in function supported natively by the engine. At the time of writing, there are over [150 such
    functions](https://github.com/mongodb/mongo/blob/06a931ffadd7ce62c32288d03e5a38933bd522d3/src/mongo/db/exec/sbe/expressions/expression.cpp#L564-L567).
    Note that function parameters are evaluated first and then are passed as arguments to the
    function.
-   [EFail](https://github.com/mongodb/mongo/blob/06a931ffadd7ce62c32288d03e5a38933bd522d3/src/mongo/db/exec/sbe/expressions/expression.h#L511-L534):
    Represents an exception and produces a query fatal error if reached at query runtime. It supports numeric error codes and error strings.
-   [ENumericConvert](https://github.com/mongodb/mongo/blob/06a931ffadd7ce62c32288d03e5a38933bd522d3/src/mongo/db/exec/sbe/expressions/expression.h#L536-L566):
    Represents the conversion of an arbitrary value to a target numeric type.
-   [EVariable](https://github.com/mongodb/mongo/blob/06a931ffadd7ce62c32288d03e5a38933bd522d3/src/mongo/db/exec/sbe/expressions/expression.h#L281-L319)
    Provides the ability to reference a variable defined elsewhere.
-   [ELocalBind](https://github.com/mongodb/mongo/blob/06a931ffadd7ce62c32288d03e5a38933bd522d3/src/mongo/db/exec/sbe/expressions/expression.h#L463-L485)
    Provides the ability to define multiple variables in a local scope. They are particularly useful
    when we want to reference some intermediate value multiple times.
-   [ELocalLambda](https://github.com/mongodb/mongo/blob/06a931ffadd7ce62c32288d03e5a38933bd522d3/src/mongo/db/exec/sbe/expressions/expression.h#L487-L507)
    Represents an anonymous function which takes a single input parameter. Many `EFunctions` accept
    these as parameters. A good example of this is the [`traverseF`
    function](https://github.com/mongodb/mongo/blob/06a931ffadd7ce62c32288d03e5a38933bd522d3/src/mongo/db/exec/sbe/vm/vm.cpp#L1329-L1357):
    it accepts 2 parameters: an input and an `ELocalLambda`. If the input is an array, the
    `ELocalLambda` is applied to each element in the array, otherwise, it is applied to the input on
    its own.

EExpressions cannot be executed directly. Rather, [they are
compiled](https://github.com/mongodb/mongo/blob/06a931ffadd7ce62c32288d03e5a38933bd522d3/src/mongo/db/exec/sbe/expressions/expression.h#L81-L84)
into executable
[`ByteCode`](https://github.com/mongodb/mongo/blob/06a931ffadd7ce62c32288d03e5a38933bd522d3/src/mongo/db/exec/sbe/vm/vm.h#L1382)
in the SBE Virtual Machine, or VM. SBE `ByteCode` execution closely resembles the [fetch, decode,
execute](https://github.com/mongodb/mongo/blob/06a931ffadd7ce62c32288d03e5a38933bd522d3/src/mongo/db/exec/sbe/vm/vm.cpp#L9638-L9641)
cycle in assembly/machine code execution. In particular, EExpressions are compiled to [a linear
buffer of
instructions](https://github.com/mongodb/mongo/blob/06a931ffadd7ce62c32288d03e5a38933bd522d3/src/mongo/db/exec/sbe/vm/vm.h#L1356-L1357)
and execution state is represented by a [program
counter](https://github.com/mongodb/mongo/blob/06a931ffadd7ce62c32288d03e5a38933bd522d3/src/mongo/db/exec/sbe/vm/vm.cpp#L9635-L9638)
(a pointer into the instruction buffer, which is computed by taking the address of the buffer and
adding an offset to it) and [a
stack](https://github.com/mongodb/mongo/blob/06a931ffadd7ce62c32288d03e5a38933bd522d3/src/mongo/db/exec/sbe/vm/vm.h#L2192-L2199)
which maintains values produced during execution. Generally speaking, instructions will obtain their
arguments by popping arguments off of the stack and will push the result of evaluation onto the
stack.

For more details about the VM, including how `EExpression` compilation and `ByteCode` execution work
in detail, please reference [the Virtual Machine section below](#virtual-machine).

## Slots

To make use of SBE values (either those produced by executing `ByteCode`, or those maintained
elsewhere), we need a mechanism to reference them throughout query execution. This is where slots
come into play: A slot is a mechanism for reading and writing values at query runtime. Each slot is
[uniquely identified by a numeric
SlotId](https://github.com/mongodb/mongo/blob/06a931ffadd7ce62c32288d03e5a38933bd522d3/src/mongo/db/exec/sbe/values/slot.h#L41-L48).
Put another way, slots conceptually represent values that we care about during query execution,
including:

-   Records and RecordIds retrieved from a collection
-   The result of evaluating an expression
-   The individual components of a sort key (where each component is bound to its own slot)
-   The result of executing some computation expressed in the input query

SlotIds by themselves don't provide a means to access or set values, rather, [slots are associated
with
SlotAccessors](https://github.com/mongodb/mongo/blob/06a931ffadd7ce62c32288d03e5a38933bd522d3/src/mongo/db/exec/sbe/values/slot.h#L50-L55),
which provide the API to read the values bound to slots as well as to write new values into slots.
There are several types of SlotAccessors, but the most common are the following:

-   The
    [`OwnedValueAccessor`](https://github.com/mongodb/mongo/blob/06a931ffadd7ce62c32288d03e5a38933bd522d3/src/mongo/db/exec/sbe/values/slot.h#L113-L212)
    allows for ownership of values. That is, this accessor is responsible for constructing/destructing
    values (in the case of deep values, this involves allocating/releasing memory). Note that an
    `OwnedValueAccessor` _can_ own values, but is not required to do so.
-   The
    [`ViewOfValueAccessor`](https://github.com/mongodb/mongo/blob/06a931ffadd7ce62c32288d03e5a38933bd522d3/src/mongo/db/exec/sbe/values/slot.h#L81-L111)
    provides a way to read values that are owned elsewhere.

While a value bound to a slot can only be managed by a single `OwnedValueAccessor`, any number of
`ViewOfValueAccessors` can be initialized to read the value associated with that slot.

A good example of the distinction between these two types of SlotAccessors is a query plan which
performs a blocking sort over a collection scan. Suppose we are scanning a restaurants collection
and we wish to find the highest rated restaurants. Such a query execution plan might look like:

```
sort [output = s1] [sortBy = s2]
scan [s1 = recordSlot, s2 = "rating"]
```

The `scan` will produce records that it reads while the `sort` will output said records according to
a specified sort order over a subset of fields in each record (in this case, ordered by the value of
the "rating" field in descending order). Note that the `scan` will also maintain pointers to values
corresponding to specific fields in each record (in this example, we are interested in the value of
the "rating" field, bound to s2). In terms of SlotAccessors, the `scan` would have two
`OwnedValueAccessors`: one for the slot corresponding to the Records that it produces and another
for the value of "rating" field that we will later sort by. The `sort`, on the other hand, would
have two `ViewOfValueAccessors` referencing the same slots that provide read-only access. While the
collection scan is responsible for producing and maintaining records and pointers to the values of
the any relevant fields, the blocking sort will only read them.

## Plan Stages

PlanStages are the final component of SBE and tie the previously discussed concepts together: they
perform query execution using values, EExpressions, and slots. They are the nodes which form an SBE
execution tree when combined. PlanStages are pull-based in that they pull data from their child
stages (as opposed to a push-based model where stages offer data to parent stages).

A single `PlanStage` may have any number of children and performs some action, implements some algorithm,
or maintains some execution state, such as:

-   Computing values bound to slots
-   Managing the lifetime of values in slots
-   Executing compiled `ByteCode`
-   Buffering values into memory

SBE PlanStages also follow an iterator model and perform query execution through the following steps:

-   First, a caller prepares a PlanStage tree for execution by calling `prepare()`.
-   Once the tree is prepared, the caller then calls `open()` to initialize the tree with any state
    needed for query execution. Note that this may include performing actual execution work, as is
    done by stages such as `HashAggStage` and `SortStage`.
-   With the PlanStage tree initialized, query execution can now proceed through iterative calls to
    `getNext()`. Note that the result set can be obtained in between calls to `getNext()` by reading
    values from slots.
-   Finally, `close()` is called to indicate that query execution is complete and release resources.

The following subsections describe [the PlanStage API](https://github.com/mongodb/mongo/blob/06a931ffadd7ce62c32288d03e5a38933bd522d3/src/mongo/db/exec/sbe/stages/stages.h#L557-L651) introduced above in greater detail:

### `virtual void prepare(CompileCtx& ctx) = 0;`

This method prepares a `PlanStage` (and, recursively, its children) for execution by:

-   Performing slot resolution, that is, obtaining `SlotAccessors` for all slots that this stage
    references and verifying that all slot accesses are valid. Typically, this is done by asking
    child stages for a `SlotAccessor*` via `getAccessor()`.
-   Compiling `EExpressions` into executable `ByteCode`. Note that `EExpressions` can reference slots
    through the `ctx` parameter.

### `virtual value::SlotAccessor* getAccessor(CompileCtx& ctx, value::SlotId slot) = 0;`

This method is called during `prepare()` and returns a SlotAccessor for `slot` if this PlanStage has
an `OwnedValueAccessor` for this slot or if the Slot is tracked by `ctx`. Note that `ctx` tracks
correlated slots (slots made accessible by a parent stage from one child stage to another) as well
as slots global to the entire plan. If the `PlanStage` has children, it may recursively invoke
`getAccessor()` on its children to attempt to obtain an accessor. Note that it is not always the
case that a parent stage can access all slots exported by descendent stages lower in the tree. A
good example of this is the
[`HashAggStage`](https://github.com/mongodb/mongo/blob/06a931ffadd7ce62c32288d03e5a38933bd522d3/src/mongo/db/exec/sbe/stages/hash_agg.cpp#L273):
stages above a `HashAggStage` in the tree parent stages cannot access slots below the
`HashAggStage`. This is because this stage will exhaust its PlanStage subtree, which renders all
slots in said subtree invalid. For more details on slot resolution, consult [the corresponding
section](#slot-resolution).

### `virtual void open(bool reOpen) = 0;`

### `virtual void close() = 0;`

These two methods mirror one another. While `open()` acquires necessary resources before `getNext()`
can be called (that is, before `PlanStage` execution can begin), `close()` releases any resources
acquired during `open()`. Acquiring resources for query execution can include actions such as:

-   Opening storage engine cursors.
-   Allocating memory.
-   Populating a buffer with results by exhausting child stages. This is often done by blocking stages
    which require processing their input to produce results. For example, the
    [`SortStage`](https://github.com/mongodb/mongo/blob/06a931ffadd7ce62c32288d03e5a38933bd522d3/src/mongo/db/exec/sbe/stages/sort.cpp#L340-L349)
    needs to sort all of the values produced by its children (either in memory or on disk) before it
    can produce results in sorted order.

It is only legal to call `close()` on PlanStages that have called `open()`, and to call `open()` on
PlanStages that are closed. In some cases (such as in
[`LoopJoinStage`](https://github.com/mongodb/mongo/blob/06a931ffadd7ce62c32288d03e5a38933bd522d3/src/mongo/db/exec/sbe/stages/loop_join.cpp#L158)),
a parent stage may `open()` and `close()` a child stage repeatedly. However, doing so may be
expensive and ultimately redundant. This is where the `reOpen` parameter of `open()` comes in: when
set to `true`, it provides the opportunity to execute an optimized a sequence of `close()` and
`open()` calls.

A good example of this is the [HashAgg
stage](https://github.com/mongodb/mongo/blob/06a931ffadd7ce62c32288d03e5a38933bd522d3/src/mongo/db/exec/sbe/stages/hash_agg.cpp#L426):
calling `HashAgg::open()` involves draining a child stage and buffering values into a hash table.
Closing the plan and then immediately opening it would involve destroying the internal hash table
and then reconstructing it, which wastes a lot of work. Instead, calling `open(reOpen = true)`
simply resets `HashAgg`'s iterator to the beginning of its hash table.

### `virtual PlanState getNext() = 0;`

The method is the main driver of query execution. The behavior of `PlanStage::getNext()` depends on
the semantics of the particular stage, but generally speaking, a `PlanStage` will call `getNext()`
on child stages as needed and update the values held in slots that belong to it. It returns
`ADVANCED` to indicate that `getNext()` can still be called and `IS_EOF` to indicate that no more
calls to `getNext()` can be made (that is, this `PlanStage` has completed execution). Importantly,
`PlanStage::getNext()` does _not_ return any results directly. Rather, it updates the state of
slots, which can then be read to obtain the results of the query.

At the time of writing, there are 36 PlanStages. As such, only a handful of common stages'
`getNext()` implementations are described below:

### [ScanStage::getNext()](https://github.com/mongodb/mongo/blob/06a931ffadd7ce62c32288d03e5a38933bd522d3/src/mongo/db/exec/sbe/stages/scan.cpp#L439)

Advances a storage cursor over a collection. This stage can function both as a 'scan' (read the
contents of a collection from start to finish) or as a 'seek' (position the cursor to a specific
RecordId, and read until EOF or a RecordId upper bound). It returns `IS_EOF` if the cursor is
exhausted or if the underlying storage cursor has advanced beyond the seek bounds. ScanStage [owns
slots for the Record and RecordId
returned](https://github.com/mongodb/mongo/blob/06a931ffadd7ce62c32288d03e5a38933bd522d3/src/mongo/db/exec/sbe/stages/scan.h#L393-L398)
from the cursor and will update them on each call to `getNext()` if these slots are defined.

The ScanStage [supports binding the values of top level fields from records to
slots](https://github.com/mongodb/mongo/blob/06a931ffadd7ce62c32288d03e5a38933bd522d3/src/mongo/db/exec/sbe/stages/scan.cpp#L584-L640).
This is a very useful optimization as it saves parent stages from having to perform a linear time
lookup over the input BSON for each top level field.

### [IndexScanStageBase::getNext()](https://github.com/mongodb/mongo/blob/06a931ffadd7ce62c32288d03e5a38933bd522d3/src/mongo/db/exec/sbe/stages/ix_scan.cpp#L289-L345)

Advances a storage cursor over an index. Note that this PlanStage is abstract and must [be derived
from to describe how to seek the
index](https://github.com/mongodb/mongo/blob/06a931ffadd7ce62c32288d03e5a38933bd522d3/src/mongo/db/exec/sbe/stages/ix_scan.h#L127-L128).
Much like `ScanStage`, `IndexScanStageBase` [maintains slots for the index key and the RecordId corresponding to the
key](https://github.com/mongodb/mongo/blob/06a931ffadd7ce62c32288d03e5a38933bd522d3/src/mongo/db/exec/sbe/stages/ix_scan.h#L172-L186).
It also has an optimization that allows for [binding a subset of the components of the index key
returned by the index to
slots](https://github.com/mongodb/mongo/blob/dbbabbdc0f3ef6cbb47500b40ae235c1258b741a/src/mongo/db/exec/sbe/values/value.cpp#L889-L929).

### [FilterStage::getNext()](https://github.com/mongodb/mongo/blob/06a931ffadd7ce62c32288d03e5a38933bd522d3/src/mongo/db/exec/sbe/stages/filter.h#L127-L141)

Calls `getNext()` on its child stage until the compiled ByteCode produces a 'true' value. Note that
the compiled bytecode logically corresponds to a filter expression.

### [LoopJoinStage::getNext()](https://github.com/mongodb/mongo/blob/06a931ffadd7ce62c32288d03e5a38933bd522d3/src/mongo/db/exec/sbe/stages/loop_join.cpp#L163-L200)

Implements a join over an outer subplan and an inner subplan. Though it implements the Nested Loop
Join algorithm, it is not necessarily used to implement a `$lookup` or even a traditional join,
rather, it models a runtime loop. More precisely, for every call to `getNext()` on the outer stage,
`LoopJoinStage` reopens the inner stage and calls `getNext()` on it. The inner stage is iterated on
subsequent `getNext()` calls until `IS_EOF` is returned. This stage supports [Right, Left, and Outer
Joins](https://github.com/mongodb/mongo/blob/dbbabbdc0f3ef6cbb47500b40ae235c1258b741a/src/mongo/db/exec/sbe/stages/loop_join.h#L47).

Note that slots from the outer stage can be made visible to [inner stage via
LoopJoinStage::\_outerCorrelated](https://github.com/mongodb/mongo/blob/06a931ffadd7ce62c32288d03e5a38933bd522d3/src/mongo/db/exec/sbe/stages/loop_join.cpp#L105-L107),
which adds said slots to the `CompileCtx` during `prepare()`. Conceptually, this is similar to the
rules around scoped variables in for loops in many programming languages:

```
for (let [outerSlot1, outerSlot2] of outerSlots) {
  let res1 = [outerSlot1, outerSlot2, innerSlot1, innerSlot2]; // Illegal
    for (let [innerSlot1, innerSlot2] of innerSlots) {
      let res2 = [outerSlot1, outerSlot2, innerSlot1, innerSlot2]; // Legal
    }
}
```

In the example above, the declaration of `res1` is invalid because values on the inner side are not
visible outside of the inner loop, while the declaration of `res2` is valid because values on the
outer side are visible to the inner side.

Also note that in the example above, the logical result of `LoopJoinStage` is a pairing of the tuple
of slots visible from the outer side along with the tuple of slots from the inner side.

# Example of SBE Plan Execution

Suppose that we have a 'alumni' collection with the following contents...

```
Documents:
{ "_id" : 0, "name" : "Mihai Andrei", "major" : "Computer Science", "year": 2019}
{ "_id" : 1, "name" : "Jane Doe", "major" : "Computer Science", "year": 2020}

Indexes:
{"major": 1}
```

...and that we wish to find all computer science majors that graduated in 2020:

```
db.alumni.find({"major" : "Computer Science", "year": 2020});
```

The query plan chosen by the classic optimizer, represented as a `QuerySolution` tree, to answer this query is as follows:

```
{
    "stage" : "FETCH",
    "planNodeId" : 2,
    "filter" : {
        "year" : {
            "$eq" : 2020
        }
    },
    "inputStage" : {
        "stage" : "IXSCAN",
        "planNodeId" : 1,
        "keyPattern" : {
            "major" : 1
        },
        "indexName" : "major_1",
        "isMultiKey" : false,
        "multiKeyPaths" : {
            "major" : [ ]
        },
        "isUnique" : false,
        "isSparse" : false,
        "isPartial" : false,
        "indexVersion" : 2,
        "direction" : "forward",
        "indexBounds" : {
            "major" : [
              "[\"Computer Science\", \"Computer Science\"]"
          ]
        }
    }
}
```

In particular, it is an `IXSCAN` over the `{"major": 1}` index, followed by a `FETCH` and a filter of
`year = 2020`. The SBE plan (generated by the [SBE stage builder](#sbe-stage-builder) with the [plan
cache](#sbe-plan-cache) disabled) for this query plan is as follows:

```
*** SBE runtime environment slots ***
$$RESULT=s7 env: { s1 = Nothing (nothing), s6 = {"major" : 1} }

*** SBE stages ***
[2] filter {traverseF(s9, lambda(l101.0) { ((move(l101.0) == 2020L) ?: false) }, false)}
[2] nlj inner [] [s2, s3, s4, s5, s6]
    left
        [1] ixseek <LowKey> <HighKey> s5 s2 s3 s4 [] @<UUID> @"major_1" true
    right
        [2] limit 1
        [2] seek s2 s7 s8 s3 s4 s5 s6 none none [s9 = year] @<uuid> true false
```

Note that `SBE runtime environment slots` denote slots which are global to the plan. Also note that
the numbers in brackets correspond to the `QuerySolutionNode` that each SBE `PlanStage` maps to
(that is, nodes with `[1]` correspond to the `IXSCAN`, while those with `[2]` correspond to the
`FETCH`).

We can represent the state of query execution in SBE by a table that shows the values bound to slots
at a point in time:

| Slot | Name                      | Value     | Owned by |
| ---- | ------------------------- | --------- | -------- |
| s2   | Seek RID slot             | `Nothing` | `ixseek` |
| s5   | Index key slot            | `Nothing` | `ixseek` |
| s7   | Record slot               | `Nothing` | `seek`   |
| s8   | RecordId slot             | `Nothing` | `seek`   |
| s9   | Slot for the field 'year' | `Nothing` | `seek`   |

Initially, all slots hold a value of `Nothing`. Note also that some slots have been omitted for
brevity, namely, s3, s4, and s6 (which correspond to a `SnapshotId`, an index identifier and an
index key pattern, respectively). These slots are used to implement the index key consistency and
corruption checks and as such, are beyond the scope of this example (see the [yielding
section](#yielding) for more information on these checks).

Execution starts by calling `getNext()` on the `filter` stage, which will call `getNext()` on its
child `nlj` stage. `nlj` will call `getNext()` once on its outer child (the `ixseek` stage) before
calling `getNext()` on the inner `seek` stage. Following the specified index bounds, the `ixseek`
will seek to the `{"": "Computer Science"}` index key and fill out slots `s2` and `s5`. At this
point, our slots are bound to the following values:

| Slot | Name                      | Value                      | Owned by |
| ---- | ------------------------- | -------------------------- | -------- |
| s2   | Seek RID slot             | `<RID for _id: 0>`         | `ixseek` |
| s5   | Index key slot            | `{"": "Computer Science"}` | `ixseek` |
| s7   | Record slot               | `Nothing`                  | `seek`   |
| s8   | RecordId slot             | `Nothing`                  | `seek`   |
| s9   | Slot for the field 'year' | `Nothing`                  | `seek`   |

After `ixseek` returns `ADVANCED`, `nlj` will call `getNext` on the child `limit` stage, which will
return `IS_EOF` after one call to `getNext()` on `seek` (in this way, a `limit 1 + seek` plan
executes a logical `FETCH`). The ScanStage will seek its cursor to the RecordId for `_id: 0` (note
that RID is not the same as `_id`), bind values to slots for the RecordId, Record, and the value for
the field 'year', and finally return `ADVANCED`. Our slots now look like so:

| Slot | Name                      | Value                                                                                      | Owned by |
| ---- | ------------------------- | ------------------------------------------------------------------------------------------ | -------- |
| s2   | Seek RID slot             | `<RID for _id: 0>`                                                                         | `ixseek` |
| s5   | Index key slot            | `{"": "Computer Science"}`                                                                 | `ixseek` |
| s7   | Record slot               | `{ "_id" : 0, "name" : "Mihai Andrei",`<br />`"major" : "Computer Science", "year": 2019}` | `seek`   |
| s8   | RecordId slot             | `<RID for _id: 0>`                                                                         | `seek`   |
| s9   | Slot for the field 'year' | `2019`                                                                                     | `seek`   |

Note that although `s8` and `s2` hold the same value (the RecordId for `_id: 0`), they represent
different entities. `s2` holds the starting point for our `seek` stage (provided by `ixseek`),
whereas `s8` holds the RecordId of the last Record read. Also note that `s7` holds the last Record
read, which is also surfaced externally as the query result (provided that our `filter` passes).

`seek` will return control to `nlj`, which returns control to `filter`. We now have a value for `s9`
and can execute our filter expression. When executing the `ByteCode` for our filter expression with
`s9` bound to 2019, the result is `false` because 2019 is not equal to 2020. As such, the `filter`
stage must call `getNext()` on `nlj` once more.

The slot tables which result from the next call to `FilterStage::getNext()` are left as an exercise
to the reader.

# Miscellaneous

## SBE Plan Cache

There exists a plan cache for SBE; see the [relevant
README](https://github.com/mongodb/mongo/blob/06a931ffadd7ce62c32288d03e5a38933bd522d3/src/mongo/db/query/README.md#sbe-plan-cache)
for more details.

## Incomplete Sections Below (TODO)

## Runtime Planners

Outline:

### `MultiPlanner`

### `CachedSolutionPlanner`

### `SubPlanner`

## Virtual Machine

Outline:

-   Compilation of EExpressions
    -   Frames/Labels
-   ByteCode Execution
    -   Dispatch of instructions
    -   Parameter resolution
    -   Management of values

## Slot Resolution

Outline:

-   Binding reflectors
-   Other SlotAccessor types (`SwitchAccessor`, `MaterializedRowAccessor`)

## Yielding

Outline:

-   What is yielding and why we yield
-   `doSaveState()/doRestoreState()`
-   Index Key Consistency/Corruption checks

## Block Processing
