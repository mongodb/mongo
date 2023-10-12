/**
 * Any query that uses getMore is vulnerable to failures that result from the query being killed by
 * a stepdown or similar process before its cursor is exhausted. This includes any aggregation on a
 * sharded cluster, because the mongos always uses getMore to get results from shards.
 *
 * Workloads that issue queries requiring multiple batches or sharded execution plans should detect
 * these errors and ensure that they do not get reported as test failures.
 */
export var interruptedQueryErrors = [
    ErrorCodes.CursorNotFound,
    ErrorCodes.CursorKilled,
    ErrorCodes.Interrupted,
    ErrorCodes.QueryPlanKilled
];