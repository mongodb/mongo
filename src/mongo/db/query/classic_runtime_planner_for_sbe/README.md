# Classic Runtime Planning for SBE

Runtime planning is an algorithm for plan selection. It runs a set of candidate plans for a trial
period, measures their productivity based on the number of documents returned and picks the most
productive plan as the result.

Classic query execution engine uses [PlanStage::work()](https://github.com/mongodb/mongo/blob/bec23e4e782bae764122dfa1931cd0d2ad5a1e07/src/mongo/db/exec/plan_stage.h#L207) as the main entry point. This method has a semantic of "do some small unit of work" and returns
one of 3 results:

- ADVANCED - I have returned a document
- NEED_TIME - no document is returned, but I did some work
- IS_EOF - I am done

This makes runtime planning easy: just call work() in a round-robin and count ADVANCED / (number of
works) ratio as productivity.

SBE query execution engine (see [README](https://github.com/mongodb/mongo/blob/bec23e4e782bae764122dfa1931cd0d2ad5a1e07/src/mongo/db/exec/sbe/README.md))
while better at executing plans, doesn't support this algorithm well.

SBE's entry point is [sbe::PlanStage::getNext()](https://github.com/mongodb/mongo/blob/bec23e4e782bae764122dfa1931cd0d2ad5a1e07/src/mongo/db/exec/sbe/stages/stages.h#L743)
method that has only two results: ADVANCED or IS_EOF. Which means that unlike work(), a call to
getNext() may take an arbitrary large amount of time. Especially if there are blocking stages like
SORT.

This means any attempt to round-robin between SBE plans can take time proportional to the longest
plan, instead of the shortest plan.

As a consequence we have to lower the trial period limits to keep planning time in check, which
declines the quality of selected plans.

To get both performance benefits of SBE engine and quality of runtime planning from Classic engine,
we use Classic Runtime Planners and execute the best plan using SBE engine.

The project uses SBE plan cache to avoid expensive SBE stage builders. Classic plan cache is not used.

## Planners

Depending on the query and current state of the database, there are 4 main cases for planning.
Each case represented as a [PlannerInterface](https://github.com/mongodb/mongo/blob/bec23e4e782bae764122dfa1931cd0d2ad5a1e07/src/mongo/db/query/planner_interface.h#L77)
implementation.

### `SingleSolutionPassthroughPlanner`

If there is only one solution:

1. Builds SBE plan.
2. Creates pinned cache entry for it.
3. Returns SBE plan executor.

"Pinned" means the query won't be considered for replanning; it's appropriate here because this is currently the only possible plan.

### `MultiPlanner`

If there are multiple solutions:

1. Builds Classic plans for each solution.
2. Uses [MultiPlanStage::pickBestPlan()](https://github.com/mongodb/mongo/blob/bec23e4e782bae764122dfa1931cd0d2ad5a1e07/src/mongo/db/exec/multi_plan.h#L115)
   to pick the best solution.
   - The multi-plan stage is configured not to read or write the Classic plan cache.
3. If aggregation pipeline is present, extends the solution with it.
4. Builds and caches SBE plan for the best solution.
5. If best solution reached EOF during planning and there is no aggregation pipeline, returns the existing Classic plan executor, which just outputs the documents already found during multiplanning.
6. Otherwise, returns SBE plan executor that will restart the query from scratch in SBE.

### `SubPlanner`

Subplanning is a process where each clause in a rooted $or query is planned separately.
For example, in match expression `{$or: [{a: 1}, {b: 1}]}`parts`{a: 1}`and`{b: 1}` will be
planned independently. This is determined by [SubplanStage::canUseSubplanning()](https://github.com/mongodb/mongo/blob/59bfa0cc51bfbdaf0cde7184e63db77f5015c0a6/src/mongo/db/exec/subplan.cpp#L81)

If subplanning can be used:

1. Uses [SubplanStage::pickBestPlan()](https://github.com/mongodb/mongo/blob/bec23e4e782bae764122dfa1931cd0d2ad5a1e07/src/mongo/db/exec/subplan.h#L129)
   to pick the best solution.
2. If aggregation pipeline is present, extends the solution with it.
3. Creates pinned cache entry for it.
4. Returns SBE plan executor.

### `CachedSolutionPlanner`

If there is an active cache entry for the query:

1. Recovers SBE plan from the cache.
2. Sets a read budget for the plan, based on cached entry previous performance.
3. Runs a trial period for the cached plan.
4. If plan is able to produce enough documents within read budget, then continues execution of this
   plan.
5. If aggregation pipeline is present, tracks the amount of documents, returned by the
   "find" part of the plan, not the whole plan, because only "find" part participated in runtime
   planning.
6. Otherwise, replans the query using [MultiPlanner](#multiplanner).
