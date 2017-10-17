The tests in this directory are rollback tests that existed in 3.4 and are intended to succeed using
the older `rollbackViaRefetchNoUUID` algorithm. The newer rollback algorithm, `rollbackViaRefetch`,
introduced in 3.6, adds the ability to roll back a wider range of operations, but the
`rollbackViaRefetchNoUUID` algorithm was maintained for multi-version replica set support. The older
`rollbackViaRefetchNoUUID` algorithm should provide guarantees no worse than the 3.4 rollback
algorithm, so we keep these old tests around to run only with that algorithm. They represent a
snapshot of 3.4 rollback tests that are related to rollback of specific operations or operation
sequences. Newer tests have been added in the main `jstests/replsets` directory that fully exercise
the `rollbackViaRefetch` algorithm, but some of those tests exercise functionality that the older
algorithm does not support. So, we maintain these tests to provide the basic assurance that the
`rollbackViaRefetchNoUUID` algorithm does not regress at all from the 3.4 rollback algorithm.

Some rollback tests that exercise general, topological scenarios exist in the main
`jstests/replsets` directory, and will be run on both algorithms, since they shouldn't depend on a
rollback method's support of specific operation types.

This suite of tests runs on the `replica_sets_rollback_refetch_no_uuid` suite. This suite, and this
sub-directory of tests, can be deleted in 3.8, when the `rollbackViaRefetchNoUUID` algorithm will no
longer be needed. See SERVER-29766.
