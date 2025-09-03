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

Yielding means releasing database locks during the execution of a query, i.e. locks on storage resources. This includes unlocking the storage snapshot being used for the query. It is done to prevent long-running queries from hogging locks and snapshots that would significantly block progress by other queries and/or prevent storage from reclaiming the space from old snapshots. It is possible that after a yield, if the snapshot previously being used is no longer available, the query may be unable to resume and therefore will fail.

Queries are intended to yield at least every 10 ms by default. Yielding is configurable via the **internalQueryExecYieldPeriodMS** and **internalQueryExecYieldIterations** [query knobs](https://github.com/mongodb/mongo/blob/60cb097488030c1b3e3096073a96cbeff603458d/src/mongo/db/query/query_knobs.idl#L399-L413), respectively.

**PlanExecutorImpl** and **PlanExecutorSBE**, in the mongo::PlanStage or sbe::PlanStage stages they are executing, yield by calling down into **PlanYieldPolicy::yieldOrInterrupt()**, which itself may call down into **PlanYieldPolicy::performYield()** or **PlanYieldPolicy::performYieldWithAcquisitions()**, which perform the actual yields as follows:

- performYield() → locker->saveLockStateAndUnlock() – yields by releasing the locks
- performYield() → locker->restoreLockState() – “unyields” by reacquiring the locks
  <br>
- performYieldWithAcquisitions() → yieldTransactionResourcesFromOperationContext() → getLocker(opCtx)->saveLockStateAndUnlock() – yields by releasing the locks
- performYieldWithAcquisitions() → restoreTransactionResourcesToOperationContext() → getLocker(opCtx)->restoreLockState() – “unyields” by reacquiring the locks

**PlanExecutorPipeline** does not yield directly. Only the parts of the pipeline that are executed by a PlanExecutorImpl or PlanExecutorSBE under the PlanExecutorPipeline may yield (see the Executors section). This means that DocumentSourceStages other than DocumentSourceCursor, which can own a sub-executor, do not themselves yield, however, due to the batching behavior at the interface between the $match execution and the rest of the pipeline, these other DocumentSource stages are normally not holding any locks when they execute, so yielding is normally not needed.

### Checking for Interrupts

Interrupts signal that the query should be killed immediately. Queries are intended to check for interrupts at least every 10 ms.

**PlanExecutorImpl** and **PlanExecutorSBE**, in the mongo::PlanStage or sbe::PlanStage stages they are executing, check for interrupts by calling **PlanYieldPolicy::yieldOrInterrupt()**, which itself may call down into **PlanYieldPolicy::performYield()** or **PlanYieldPolicy::performYieldWithAcquisitions()**, which in turn may call **OperationContext::checkForInterrupt()**, which performs the interrupt check.

**PlanExecutorPipeline**, besides the interrupt checks done by any PlanExecutorImpl and/or PlanExecutorSBE instances it owns, also may check for interrupts directly in its call chain:

1. PlanExecutorPipeline::getNext()
2. PlanExecutorPipeline::getNextDocument()
3. PlanExecutorPipeline::\_getNext()
4. PlanExecutorPipeline::\_tryGetNext()
5. Pipeline::getNext() (from PlanExecutorPipeline::\_pipeline)
6. DocumentSource::getNext() (from Pipeline::\_sources)
7. ExpressionContext::checkForInterrupt() (from DocumentSource::pExpCtx)
8. ExpressionContext::checkForInterruptSlow()
9. OperationContext::checkForInterrupt() (from ExpressionContext::opCtx)
   (Aintcha glad I found that whole chain for ya?😀)

### PlanYieldPolicy and Its Children

PlanYieldPolicy is an abstract base class.

- **PlanExecutorImpl** (Classic find) uses concrete child **PlanYieldPolicyImpl**.
- **PlanExecutorSBE** uses concrete child **PlanYieldPolicySBE**.
- **PlanExecutorPipeline** does not have a yield policy type as it does not yield directly.

In Classic find, the PlanExecutorImpl owns its PlanYieldPolicyImpl, but the SBE story is more complex.

In SBE a PlanExecutorSBE owns a PlanYieldPolicySBE (in a std::unique_ptr\<PlanYieldPolicySBE\>), but in the sbe::PlanStage tree, each PlanStage node may also be given a raw C++ pointer to this PlanYieldPolicySBE if that stage is meant to yield. Nodes that are not meant to yield are given a null raw pointer. The individual PlanStages call yielding methods via their local yield policy pointers if non-null.

It is important to understand that the lifespans of the sbe::PlanStage tree, PlanExecutorSBE, and PlanYieldPolicySBE are all **independent of each other**, and that while a PlanExecutorSBE nominally owns both a PlanStage tree and a PlanYieldPolicySBE, the PlanStage tree may outlive the executor. In particular, the same PlanStage tree gets passed from one executor to another between the trial and run phases when using Classic runtime planner for SBE. At this handoff point, the old executor and yield policy are destructed, a new yield policy is created, and a call to **prepareSlotBasedExecutableTree()** is responsible for walking the entire PlanStage tree and replacing all of the old non-null yield policy pointers (which now point to freed memory) with new pointers that point to the new executor’s new yield policy object, otherwise those stages would try to access the old, freed yield policy object. (Any stages that had a null yield policy pointer keep their null pointer and continue not to yield.)

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
