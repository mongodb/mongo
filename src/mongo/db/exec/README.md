# Query Execution

## Storage resources lifecycle and yielding

Query execution requires usage of lower level storage resources like locks, snapshots, cursors, etc.
The lifecyle of those resources depends on the execution model used.

There is an expectation that resources are not held for a prolonged period of time. The resources
that are held may pin data (when MVCC is used) or may prevent log truncation, therefore it is
important that they are periodically released and reacquired.

### DocumentSource

[`mongo::DocumentSource`](https://github.com/10gen/mongo/blob/master/src/mongo/db/pipeline/document_source.h#L198)
execution model currently doesn't account for longer lived storage resources.

Due to lack of yielding support, all acquired resources must have a strictly bounded lifecycle, e.g.

storage resources are not allowed to leave `DocumentSource::doGetNext` method scope, if acquired
there.

### DocumentSourceCursor

[`mongo::DocumentSourceCursor`](https://github.com/10gen/mongo/blob/master/src/mongo/db/pipeline/document_source_cursor.h#L73)
acts as an adapter from [`mongo::PlanExecutor`](https://github.com/10gen/mongo/blob/master/src/mongo/db/query/plan_executor.h#L129)  
to [`mongo::DocumentSource`](https://github.com/10gen/mongo/blob/master/src/mongo/db/pipeline/document_source.h#L198). It
is responsible for aquiring and releasing the storage resources (snapshots, locks, etc.), so that
they don't leave the `mongo::DocumentSourceCursor` scope. Some of the storage resources are acquired
and released by `mongo::PlanExecutor` via `mongo::PlanExecutor::restoreState` and
`mongo::PlanExecutor::saveState`

In order to avoid acquiring and releasing resources for every call to
`mongo::PlanExecutor:getNextDocument`, `mongo::DocumentSourceCursor`
performs batching, where storage resources are acquired, then multiple calls to
`mongo::PlanExecutor:getNextDocument` are performed and results are buffered, and finally resources
are released. Buffered results are then used in subsequent calls to
`DocumentSourceCursor::doGetNext`.

### Classic yielding

Classic plan executor performs yields outside of calls to work(). Classic plan stages also use
interrupt style yielding. When yield is needed work() can be interruped and a
[`mongo::PlanStage::StageState::NEED_YIELD`](https://github.com/10gen/mongo/blob/master/src/mongo/db/exec/plan_stage.h#L166-L184)
is used to unwind the plan stage call stack. After that classic plan executor
([`mongo::PlanExecutorImpl`](https://github.com/10gen/mongo/blob/master/src/mongo/db/query/plan_executor_impl.h#L118`))
will release and reaquire storage resources.

### SBE yielding

SBE plan stages use cooperative style yielding. When a yield is needed SBE plan stage performs the
yield in place, without unwinding the plan stage call stack.

In order for yielding to occur regularly, without unbounded delay, each stage
`mongo::sbe::PlanStage::getNext()` method must ensure that:

1. the check for interrupt or yield is performed at least once. And if needed perform yield in
   place.
2. does not introduce unbounded delay between checks for interrupt or yield.

Additionally `mongo::sbe::PlanStage::open()` and `mongo::sbe::PlanStage::close()` method's must
ensure that they don't not introduce unbounded delay between checks for interrupt or yield.

#### Yielding implementation guidelines

The above requirements are satisfied by the following guidelines below.

`mongo::sbe::PlanStage::getNext()` guidelines:

-   must check for interrupt or yield at least once in `getNext` method.
-   a `getNext` implementation that calls a child `getNext` method, may rely on the child to check for
    yield or interrupt.
    e.g.: A call to project, match, or unwind stage, when they call child `getNext` method.
-   a `getNext` implementation that does not call a child `getNext` method, must perform the yield or
    interrupt check, if unbounded number of such calls is possible.
    e.g.: A scan, sort, group, unwind stage, when they don't call a child `getNext` method.
-   a `getNext` implementation that performs an unbounded loop must ensure that checks for yield or
    interrupt are regularly performed,
    by either relying on child `getNext` method or by explicitly performing yield or interrupt check.

`mongo::sbe::PlanStage::open()` and `mongo::sbe::PlanStage::close()` guidelines:

-   doesn't need to perform check for yield or interrupt if its execution time is bounded.
-   an `open` or `close` implementation that performs an unbounded loop must ensure that checks for
    yield or interrupt are regularly performed, by either relying on child `getNext` method or by
    explicitly performing the yield or interrupt check.

#### Yielding and memory safety

When storage resources are released, all unowned values (views) that are backed directly by the
storage resource become invalid.
Such values must be either discarded, when no longer needed, or converted to owned values by making
their copy before the storage resources are released.

This is commongly done by using
[mongo::sbe::CanChangeState::prepareForYielding](https://github.com/10gen/mongo/blob/master/src/mongo/db/exec/sbe/stages/stages.h#L126)
utility.

When stage slots are no longer needed, a `mongo::sbe::CanTrackStats::disableSlotAccess` method can
be called. This informs the stage that the slots it produces will not be accessed until subsequent
call to `getNext`. This allows the plan stage to invalidate the eligible slots and avoid making a
copy of their contents in case storage resources are released.
