-------------------------- MODULE PipelinedOplog --------------------------
\* Core pipelined oplog applier protocol: batches may finish out of order,
\* but the advancer publishes lastApplied only through a completed prefix.
\*
\* Assumptions and scope:
\* - Each op applies atomically at its own optime.
\* - The dispatcher, worker, and advancer all make progress.
\* - Routing and classification (whether or not an op is serialized) are fixed.
\* - Registration of the batch in the inflight queue and enqueueing the batch are separate steps.
\*   All slices enqueue atomically. FIFO removal and publication are one atomic step.
\* - Backpressure is count-based; byte accounting is omitted.
\* - Transactions, deferred writes, failures, worker resize, and lifecycle transitions are omitted.

EXTENDS Naturals, Sequences, FiniteSets, TLC

\* The number of operations. Optimes are also mapped to 1..NumOps.
CONSTANT NumOps

\* The set of document identities, abstracting (namespace, _id).
CONSTANT Keys

\* The set of worker IDs.
CONSTANT Workers

\* The key touched by each operation: a function from 1..NumOps to Keys.
CONSTANT OpKey

\* The fixed routing of each key: a function from Keys to Workers. Abstracts the hash-routing.
CONSTANT WorkerOf

\* The set of operations requiring drain + inline application.
CONSTANT SerializedOps

\* The maximum number of operations the batcher can include in one batch.
CONSTANT MaxBatchSize

\* The model's per-worker item bound, abstracting count-based queue backpressure.
CONSTANT MaxQueueDepth

\* The maximum number of batches in the in-flight FIFO.
CONSTANT MaxInflight

\* Validation for constants.
ASSUME
    /\ NumOps \in Nat /\ NumOps >= 1
    /\ MaxBatchSize \in Nat /\ MaxBatchSize >= 1
    /\ MaxQueueDepth \in Nat /\ MaxQueueDepth >= 1
    /\ MaxInflight \in Nat /\ MaxInflight >= 1
    /\ SerializedOps \subseteq 1..NumOps
    /\ \A i \in 1..NumOps: OpKey[i] \in Keys
    /\ \A k \in Keys: WorkerOf[k] \in Workers

----
\* Batcher state

\* The next oplog position to include in a batch.
VARIABLE nextToBatch

\* All batches formed, in oplog order. Each record here has ops, serialized, and
\* maxOpTime fields. Its index is the batch ID.
VARIABLE batches

----
\* Dispatcher state

\* The index of the next batch to dispatch.
VARIABLE nextToDispatch

\* The dispatcher phase; see DispatcherPhases below.
VARIABLE dispatcherPhase

\* A registered batch awaiting worker enqueueing, or an inline batch awaiting its
\* synthetic FIFO entry. NoPendingBatch means neither step is outstanding.
VARIABLE pendingBatch

----
\* Worker state and batch completion tracking

\* A function from Workers to FIFO sequences of work items. Represents each worker's queue of work. Each item here has ops
\* and batch fields, with the batch linking a slice to its originating batch. The active item stays at the head until WorkerFinishItem.
VARIABLE workerQueues

\* A function from registered batch IDs to remaining-worker counts, representing the shared counters for each batch.
VARIABLE remainingWorkers

\* The set of operations applied in the storage engine.
VARIABLE applied

----
\* Advancer state

\* The FIFO queue of batches awaiting publication, with batch and maxOpTime fields.
VARIABLE inflightBatches

\* The published timestamp. LastAppliedSafety checks that all ops at or below it.
VARIABLE lastApplied

vars == <<nextToBatch, batches, nextToDispatch, dispatcherPhase, pendingBatch,
          workerQueues, remainingWorkers, inflightBatches, applied, lastApplied>>

----
\* Helpers

\* Dispatcher phases:
\*   ready         - may start dispatching the next batch
\*   enqueueItems  - batch registered; slices not yet enqueued
\*   pushSynthetic - inline op applied; synthetic entry not yet pushed
\*   waitPublish   - synthetic entry pushed; waiting for publication
DispatcherPhases == {"ready", "enqueueItems", "pushSynthetic", "waitPublish"}

\* Batch IDs start at 1, so batch 0 is a sentinel value for no pending entry.
NoPendingBatch == [batch |-> 0, maxOpTime |-> 0]

\* A batch's original operations, whether it should be serialized, and max optime in the batch.
BatchElt    == [ops: Seq(1..NumOps), serialized: BOOLEAN, maxOpTime: 1..NumOps]

\* One worker's remaining slice of operations and the ID of the counter to decrement.
ItemElt     == [ops: Seq(1..NumOps), batch: 1..NumOps]

\* Publication metadata; the shared counter is stored separately in remainingWorkers.
InflightElt == [batch: 1..NumOps, maxOpTime: 1..NumOps]

\* Function that determines the worker for a particular optime.
\* It follows the optime-to-document-key and document-key-to-worker mappings.
RoutesTo(op, w) == WorkerOf[OpKey[op]] = w

\* Registration reserves room for one new item per worker and one FIFO entry.
\* This notably omits byte-accounting in backpressure.
UnderBackpressureLimits ==
    /\ \A w \in Workers: Len(workerQueues[w]) < MaxQueueDepth
    /\ Len(inflightBatches) < MaxInflight

\* Used in the inline-entry guard to ensure prior dispatched work is applied and published.
PipelineDrained ==
    /\ \A w \in Workers: workerQueues[w] = <<>>
    /\ \A b \in DOMAIN remainingWorkers: remainingWorkers[b] = 0
    /\ inflightBatches = <<>>

----
\* Define initial values for all variables.

\* Starting state: nothing batched, dispatched, applied, or published.
Init ==
    /\ nextToBatch      = 1
    /\ batches          = <<>>
    /\ nextToDispatch   = 1
    /\ dispatcherPhase  = "ready"
    /\ pendingBatch     = NoPendingBatch
    \* Constructs a function with one empty queue per worker.
    /\ workerQueues     = [w \in Workers |-> <<>>]
    \* A function with an empty domain: no batches have counters yet.
    /\ remainingWorkers = [b \in {} |-> 0]
    /\ inflightBatches  = <<>>
    /\ applied          = {}
    /\ lastApplied      = 0

----
\* Batcher actions
\* The batcher takes contiguous ranges; serialized ops always stand alone.

\* ACTION
\* One step of the oplog batcher: cut the next 1..MaxBatchSize ops into a batch.
FormBatch ==
    \* No more batches can be formed after the oplog is exhausted.
    /\ nextToBatch <= NumOps
    \* Allow any size that fits both the oplog and the boundaries below.
    /\ \E size \in 1..MaxBatchSize:
        /\ nextToBatch + size - 1 <= NumOps
        \* Build the sequence of oplog positions; its first entry determines the path.
        /\ LET opsSeq == [j \in 1..size |-> nextToBatch + j - 1]
               isSer  == nextToBatch \in SerializedOps
           \* A serialized op stands alone; a pipelined batch cannot cross one.
           IN /\ IF isSer
                 THEN size = 1
                 ELSE \A j \in 1..size: opsSeq[j] \notin SerializedOps
              \* Keep the original batch for dispatch and invariant checks.
              /\ batches' = Append(batches,
                                   [ops        |-> opsSeq,
                                    serialized |-> isSer,
                                    maxOpTime  |-> nextToBatch + size - 1])
        \* The next batch starts immediately after this one.
        /\ nextToBatch' = nextToBatch + size
    /\ UNCHANGED <<nextToDispatch, dispatcherPhase, pendingBatch, workerQueues,
                   remainingWorkers, inflightBatches, applied, lastApplied>>

----
\* Dispatcher actions: pipelined path
\* Registration and enqueueing are separate atomic steps. Between them, the
\* advancer can see the batch, but its nonzero counter prevents publication.

\* ACTION
\* Dispatcher step 1: atomically create the shared counter and push the
\* batch onto the in-flight FIFO.
DispatchRegisterBatch ==
    /\ dispatcherPhase = "ready"
    \* Dispatch only a batch that the batcher has already formed.
    /\ nextToDispatch <= Len(batches)
    \* Serialized batches use the InlineApply action instead.
    /\ ~batches[nextToDispatch].serialized
    /\ UnderBackpressureLimits
    /\ LET b            == nextToDispatch
           ops          == batches[b].ops
           \* SelectSeq filters operations for w without changing their order.
           Slice(w)     == SelectSeq(ops, LAMBDA op: RoutesTo(op, w))
           \* Only workers with a nonempty slice contribute to the counter.
           participants == {w \in Workers: Slice(w) # <<>>}
       IN \* b :> n constructs a one-entry function; @@ combines it with the
          \* existing shared counters. b is a new key. The full count is visible
          \* in the same transition as the FIFO entry, before enqueueing.
          /\ remainingWorkers' = remainingWorkers @@ (b :> Cardinality(participants))
          /\ inflightBatches' = Append(inflightBatches,
                                       [batch |-> b, maxOpTime |-> batches[b].maxOpTime])
          \* Retain the batch ID for the separate enqueue action.
          /\ pendingBatch' = [batch |-> b, maxOpTime |-> batches[b].maxOpTime]
          \* The cursor advances, but dispatcherPhase prevents starting another batch yet.
          /\ nextToDispatch' = b + 1
          /\ dispatcherPhase' = "enqueueItems"
    /\ UNCHANGED <<nextToBatch, batches, workerQueues, applied, lastApplied>>

\* ACTION
\* Dispatcher step 2: enqueue one item per participant in the registered batch.
DispatchEnqueueItems ==
    /\ dispatcherPhase = "enqueueItems"
    /\ LET b            == pendingBatch.batch
           ops          == batches[b].ops
           Slice(w)     == SelectSeq(ops, LAMBDA op: RoutesTo(op, w))
           participants == {w \in Workers: Slice(w) # <<>>}
       IN workerQueues' = [w \in Workers |->
                             IF w \in participants
                             \* Each item carries its batch ID for completion accounting.
                             THEN Append(workerQueues[w], [ops |-> Slice(w), batch |-> b])
                             \* Leave non-participating workers' queues unchanged.
                             ELSE workerQueues[w]]
    \* Enqueueing is complete; the dispatcher can accept the next batch.
    /\ pendingBatch' = NoPendingBatch
    /\ dispatcherPhase' = "ready"
    /\ UNCHANGED <<nextToBatch, batches, nextToDispatch, remainingWorkers,
                   inflightBatches, applied, lastApplied>>

----
\* Dispatcher actions: drain + inline path
\* The dispatcher waits for prior work to be applied and published, applies the
\* serialized op, then waits for its synthetic batch to publish before resuming.

\* ACTION
\* Apply a serialized (e.g. DDL) op on the dispatcher, only when fully the workers are fully drained.
InlineApply ==
    /\ dispatcherPhase = "ready"
    /\ nextToDispatch <= Len(batches)
    /\ batches[nextToDispatch].serialized
    /\ PipelineDrained
    /\ LET b  == nextToDispatch
           \* FormBatch guarantees this serialized batch has exactly one op.
           op == batches[b].ops[1]
       \* Applying the op and creating its completed counter are one transition.
       IN /\ applied' = applied \cup {op}
          \* Zero is valid here, as the dispatcher has performed all the work.
          /\ remainingWorkers' = remainingWorkers @@ (b :> 0)
          \* Save the synthetic entry for the next dispatcher action.
          /\ pendingBatch' = [batch |-> b, maxOpTime |-> op]
          /\ nextToDispatch' = b + 1
          /\ dispatcherPhase' = "pushSynthetic"
    /\ UNCHANGED <<nextToBatch, batches, workerQueues, inflightBatches, lastApplied>>

\* ACTION
\* Submit the completed inline batch to the advancer (this does not publish it).
DispatchPushSynthetic ==
    /\ dispatcherPhase = "pushSynthetic"
    /\ inflightBatches' = Append(inflightBatches, pendingBatch)
    /\ pendingBatch' = NoPendingBatch
    /\ dispatcherPhase' = "waitPublish"
    /\ UNCHANGED <<nextToBatch, batches, nextToDispatch, workerQueues, remainingWorkers,
                   applied, lastApplied>>

\* ACTION
\* Dispatcher resumes once the advancer has published the synthetic batch.
DispatchResumeAfterPublish ==
    /\ dispatcherPhase = "waitPublish"
    /\ inflightBatches = <<>>
    /\ dispatcherPhase' = "ready"
    /\ UNCHANGED <<nextToBatch, batches, nextToDispatch, pendingBatch, workerQueues,
                   remainingWorkers, inflightBatches, applied, lastApplied>>

----
\* Worker actions
\* Each worker applies its front item in order, then completes it in a separate
\* step. It may start its next batch while another worker is still behind.  

\* ACTION
\* Worker w applies the next op of the work item at the head of its queue.
WorkerApplyOp(w) ==
    /\ workerQueues[w] # <<>>
    \* An exhausted head item must move to WorkerFinishItem before any more apply actions.
    /\ Head(workerQueues[w]).ops # <<>>
    /\ LET item == Head(workerQueues[w])
           op   == Head(item.ops)
       IN /\ applied' = applied \cup {op}
          \* EXCEPT constructs an updated value, preserving everything else.
          \* ![w][1].ops selects the head item's remaining ops; Tail removes
          \* the first operation. The item itself stays until completion.
          /\ workerQueues' = [workerQueues EXCEPT ![w][1].ops = Tail(item.ops)]
    /\ UNCHANGED <<nextToBatch, batches, nextToDispatch, dispatcherPhase, pendingBatch,
                   remainingWorkers, inflightBatches, lastApplied>>

\* ACTION
\* Worker w finished its whole item: dequeue it and decrement the batch counter.
\* Must be called after WorkerApplyOp for a particular work item.
WorkerFinishItem(w) ==
    /\ workerQueues[w] # <<>>
    \* Completion is allowed only after every operation in this slice applied.
    /\ Head(workerQueues[w]).ops = <<>>
    \* In EXCEPT, @ denotes the previous value at the selected location.
    \* The logic below decrements the counter for batch b.
    /\ LET b == Head(workerQueues[w]).batch
       IN remainingWorkers' = [remainingWorkers EXCEPT ![b] = @ - 1]
    \* Removing the item prevents this worker from completing it twice.
    /\ workerQueues' = [workerQueues EXCEPT ![w] = Tail(workerQueues[w])]
    /\ UNCHANGED <<nextToBatch, batches, nextToDispatch, dispatcherPhase, pendingBatch,
                   inflightBatches, applied, lastApplied>>

----
\* Advancer action
\* The advancer is the sole publisher. A completed later batch cannot bypass
\* an unfinished front batch; removal and publication are atomic.

\* ACTION
\* Advancer: pop the front batch iff complete, publish its max optime.
AdvancerPublishBatch ==
    /\ inflightBatches # <<>>
    \* Do not search for another completed batch if the front is unfinished.
    /\ remainingWorkers[inflightBatches[1].batch] = 0
    \* Removal and publication happen together in this abstraction.
    /\ lastApplied' = inflightBatches[1].maxOpTime
    /\ inflightBatches' = Tail(inflightBatches)
    /\ UNCHANGED <<nextToBatch, batches, nextToDispatch, dispatcherPhase, pendingBatch,
                   workerQueues, remainingWorkers, applied>>

----
\* Defines how the variables may transition. Any enabled component may take a
\* step, with workers selected through the choice of w.
Next ==
    \* --- Batching and dispatch
    \/ FormBatch
    \/ DispatchRegisterBatch
    \/ DispatchEnqueueItems
    \/ InlineApply
    \/ DispatchPushSynthetic
    \/ DispatchResumeAfterPublish
    \* --- Worker application and completion
    \/ \E w \in Workers: WorkerApplyOp(w) \/ WorkerFinishItem(w)
    \* --- Publication
    \/ AdvancerPublishBatch

\* Weak fairness requires a continuously enabled action to eventually run.
\* Each worker has its own fairness conditions. Advancer fairness assumes
\* eventual wakeup; notifications, timer ticks, and time bounds are not modeled.
Fairness ==
    /\ WF_vars(FormBatch)
    /\ WF_vars(DispatchRegisterBatch)
    /\ WF_vars(DispatchEnqueueItems)
    /\ WF_vars(InlineApply)
    /\ WF_vars(DispatchPushSynthetic)
    /\ WF_vars(DispatchResumeAfterPublish)
    /\ \A w \in Workers: WF_vars(WorkerApplyOp(w)) /\ WF_vars(WorkerFinishItem(w))
    /\ WF_vars(AdvancerPublishBatch)

\* [] means "always"; [Next]_vars permits a Next step or an unchanged state.
\* Such unchanged (stuttering) steps also allow the finished system to idle.
Spec == Init /\ [][Next]_vars /\ Fairness

----
\* Properties to check

\* Check state shapes and ranges. These are state invariants.
TypeOK ==
    /\ nextToBatch     \in 1..(NumOps+1)
    /\ batches         \in Seq(BatchElt)
    /\ nextToDispatch  \in 1..(Len(batches)+1)
    /\ dispatcherPhase \in DispatcherPhases
    \* A pending entry is either publication metadata or the sentinel.
    /\ pendingBatch    \in InflightElt \cup {NoPendingBatch}
    \* [Workers -> Seq(ItemElt)] is the set of functions of Worker to sequence of work items.
    /\ workerQueues    \in [Workers -> Seq(ItemElt)]
    \* Counters exist only for registered batches.
    /\ DOMAIN remainingWorkers \subseteq 1..Len(batches)
    \* Each batch's shared counter's upper limit is the number of workers.
    /\ \A b \in DOMAIN remainingWorkers: remainingWorkers[b] \in 0..Cardinality(Workers)
    /\ inflightBatches \in Seq(InflightElt)
    /\ applied         \subseteq (1..NumOps)
    /\ lastApplied     \in 0..NumOps

\* Every operation at or below lastApplied must have been applied. Since each op
\* commits at its own optime by assumption, its commit timestamp is also at or
\* below lastApplied.
LastAppliedSafety ==
    \A i \in 1..NumOps: i <= lastApplied => i \in applied

\* Applying an operation requires all earlier same-key ops to have applied.
\* Fixed routing and worker FIFO preserve this order.
PerKeyOrder ==
    \A i, j \in 1..NumOps:
        (i < j /\ OpKey[i] = OpKey[j] /\ j \in applied) => i \in applied

\* Every remaining operation in a worker's queue routes to that worker.
QueueRoutingOK ==
    \A w \in Workers:
        \A k \in 1..Len(workerQueues[w]):
            \A m \in 1..Len(workerQueues[w][k].ops):
                RoutesTo(workerQueues[w][k].ops[m], w)

\* Adjacent in-flight entries increase in both batch ID and publication optime.
InflightOrdered ==
    \A k \in 1..(Len(inflightBatches)-1):
        /\ inflightBatches[k].batch < inflightBatches[k+1].batch
        /\ inflightBatches[k].maxOpTime < inflightBatches[k+1].maxOpTime

\* After inline application, all dispatched ops are applied and all prior
\* batches are published. The synthetic entry may still await publication.
\* nextToDispatch-1 is the inline batch; nextToDispatch-2 is its predecessor,
\* whose optime must be the published lastApplied before the synthetic push.
InlineDrainedOK ==
    dispatcherPhase \in {"pushSynthetic", "waitPublish"} =>
        /\ \A w \in Workers: workerQueues[w] = <<>>
        /\ \A b \in DOMAIN remainingWorkers: remainingWorkers[b] = 0
        /\ \A k \in 1..(nextToDispatch-1):
              \A m \in 1..Len(batches[k].ops): batches[k].ops[m] \in applied
        /\ \A k \in 1..(nextToDispatch-2): batches[k].maxOpTime <= lastApplied
        /\ dispatcherPhase = "pushSynthetic" /\ nextToDispatch >= 3 =>
              lastApplied = batches[nextToDispatch-2].maxOpTime

\* A pending batch exists exactly between registration and enqueue on the
\* pipelined path, or between inline application and the synthetic push in the drain path.
PendingBatchCoherent ==
    (dispatcherPhase \in {"enqueueItems", "pushSynthetic"}) <=> (pendingBatch # NoPendingBatch)

\* Compare successive states; this safety condition is a temporal property.
MonotonicLastApplied == [][lastApplied' >= lastApplied]_vars

----
\* Liveness checks

\* P ~> Q means "whenever P holds, Q eventually holds". Under fairness,
\* picking up an operation for batching must eventually lead to its application.
NoStarvation ==
    \A i \in 1..NumOps: (i < nextToBatch) ~> (i \in applied)

\* <> means "eventually": publication must reach the end of the finite oplog.
EventualPublication == <>(lastApplied = NumOps)

============================================================================
