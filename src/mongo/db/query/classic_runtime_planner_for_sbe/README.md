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

In prior attempts to use SBE's execution model for planning, we had to lower the trial period limits to keep planning time in check, but that reduced the quality of selected plans.

To get both performance benefits of SBE engine and quality of runtime planning from Classic engine,
we use Classic Runtime Planners and execute the best plan using SBE engine.

## Planners

Depending on the query and current state of the database, there are 4 main cases for planning.
Each case represented as a [PlannerInterface](https://github.com/mongodb/mongo/blob/bec23e4e782bae764122dfa1931cd0d2ad5a1e07/src/mongo/db/query/planner_interface.h#L77)
implementation.

### `SingleSolutionPassthroughPlanner`

If there is only one solution:

1. Builds SBE plan.
2. Creates pinned cache entry for it, if SBE plan cache is enabled.
3. Returns SBE plan executor.

"Pinned" means the query won't be considered for replanning; it's appropriate here because this is currently the only possible plan.

### `MultiPlanner`

If there are multiple solutions:

1. Builds Classic plans for each solution.
2. Uses [MultiPlanStage::pickBestPlan()](https://github.com/mongodb/mongo/blob/bec23e4e782bae764122dfa1931cd0d2ad5a1e07/src/mongo/db/exec/multi_plan.h#L115)
   to pick the best solution.
3. If aggregation pipeline is present, extends the solution with it.
4. Builds SBE plan for the best solution.
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
3. Returns SBE plan executor.

### `ValidCandidatePlanner`

This planner is used after we've taken a plan from the cache and the trial period has ended. This planner merely continues execution of the plan that was generated from the cache entry.

## Plan Caching

Classic Runtime Planning for SBE supports both the SBE plan cache and the classic plan cache. By default, the classic plan cache is always used. The SBE plan cache is used only when `featureFlagSbeFull` is enabled.

### Writing to the Cache

Each planner is responsible for writing to the correct cache, if necessary. For example, the MultiPlanner will write to the plan cache after the underlying (classic) MultiPlanStage picks the best plan. If the SBE cache is enabled, the entire SBE plan, including any pushed-down agg pipeline, will be written. Otherwise, the find() portion of the query which was multi-planned will get written to the classic cache.

During sub-planning, with classic cache enabled, we sometimes write cache entries which are used only for future sub-planning. For example, if an OR query with an equality predicate on `a` and `b` is run, a cache entry will be written for both branches. If another OR query with a predicate on `a` and `c` is run, the cache entry for `a` may be re-used here, to avoid multi planning that branch again.

### Reading from the Cache

At the `get_executor` level, we determine which cache to read from based on the `featureFlagSbeFull` value. If a cache entry is found, a `PlannerGenerator` is then created which does the job of translating the cache entry into a `PlannerInterface` which can then be used to generate a plan. There are two `PlannerGenerator` types:

`PlannerGeneratorFromSbeCacheEntry` will use the cache entry's SBE plan to create a clone with the parameters filled in, then run the trial period.

`PlannerGeneratorFromClassicCacheEntry` will take the QSN tree for the given query + pipeline, lower it to SBE via the stage builders, and run the trial period.

The PlannerGenerators will then produce one of:

1. A `SingleSolutionPassthroughPlanner` if the cached plan is the only option.
2. A `ValidCandidatePlanner`, if the trial period was successful and we should continue using the cached plan.
3. A `MultiPlanner` if the trial was not successful, or replanning is necessary for any other reason.

After this, the returned `Planner` encapsulates any work that was cached, and we continue planning as usual.
