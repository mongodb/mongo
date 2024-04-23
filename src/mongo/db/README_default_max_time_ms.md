# Default Command Time-Out

## defaultMaxTimeMS

In addition to the query-specific `maxTimeMS`, the cluster-wide server parameter `defaultMaxTimeMS`
can be used to set the default time limit for when `maxTimeMS` is not specified. The parameter has
an effect only when authentication is turned on. The value of `defaultMaxTimeMS.readOperations`
applies to read operations. Note: aggregations with pipelines containing $out and $merge stages are
considered writes.

The cluster parameter `defaultMaxTimeMS` shares the same semantic as `maxTimeMS`. When the value is
set to 0, the default value, command runtime is unbounded. Commands that run longer than
`defaultMaxTimeMS` will return a `MaxTimeMSExpired` error.

## bypassDefaultMaxTimeMS

All commands run by a user with `bypassDefaultMaxTimeMS` privilege will ignore the value of
`defaultMaxTimeMS`. The root and \_\_system roles have the privilege by default.

## Time-Out Precedence

When multiple time-out values are available, the value is chosen using the following hierarchy
(higher to lower priority):

- The query-specific `maxTimeMS` option
- The tenant-specific `defaultMaxTimeMS` value
- The global `defaultMaxTimeMS` value
