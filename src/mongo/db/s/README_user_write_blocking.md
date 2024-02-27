# User Write Blocking

User write blocking prevents user initiated writes from being performed on C2C source and destination
clusters during certain phases of C2C replication, allowing durable state to be propagated from the
source without experiencing conflicts. Because the source and destination clusters are different
administrative domains and thus can have separate configurations and metadata, operations which
affect metadata, such as replset reconfig, are permitted. Also, internal operations which affect user
collections but leave user data logically unaffected, such as chunk migration, are still permitted.
Finally, users with certain privileges can bypass user write blocking; this is necessary so that the
C2C sync daemon itself can write to user data.

User write blocking is enabled and disabled by the command `{setUserWriteBlockMode: 1, global:
<true/false>}`. On replica sets, this command is invoked on the primary, and enables/disables user
write blocking replica-set-wide. On sharded clusters, this command is invoked on `mongos`, and
enables/disables user write blocking cluster-wide. We define a write as a "user write" if the target
database is not internal (the `admin`, `local`, and `config` databases being defined as internal),
and if the user that initiated the write cannot perform the `bypassWriteBlockingMode` action on the
`cluster` resource. By default, only the `restore`, `root`, and `__system` built-in roles have this
privilege.

The `UserWriteBlockModeOpObserver` is responsible for blocking disallowed writes. Upon any operation
which writes, this `OpObserver` checks whether the `GlobalUserWriteBlockState` [allows writes to the
target
namespace](https://github.com/mongodb/mongo/blob/387f8c0e26a352b95ecfc6bc51f749d26a929390/src/mongo/db/op_observer/user_write_block_mode_op_observer.cpp#L281-L288).
The `GlobalUserWriteBlockState` stores whether user write blocking is enabled in a given
`ServiceContext`. As part of its write access check, it [checks whether the `WriteBlockBypass`
associated with the given `OperationContext` is
enabled](https://github.com/mongodb/mongo/blob/25377181476e4140c970afa5b018f9b4fcc951e8/src/mongo/db/s/global_user_write_block_state.cpp#L59-L67).
The `WriteBlockBypass` stores whether the user that initiated the write is able to perform writes
when user write blocking is enabled. On internal requests (i.e. from other `mongod` or `mongos`
instances in the sharded cluster/replica set), the request originator propagates `WriteBlockBypass`
[through the request
metadata](https://github.com/mongodb/mongo/blob/182616b7b45a1e360839c612c9ee8acaa130fe17/src/mongo/rpc/metadata.cpp#L115).
On external requests, `WriteBlockBypass` is enabled [if the authenticated user is privileged to
bypass user
writes](https://github.com/mongodb/mongo/blob/07c3d2ebcd3ca8127ed5a5aaabf439b57697b530/src/mongo/db/write_block_bypass.cpp#L60-L63).
The `AuthorizationSession`, which is responsible for maintaining the authorization state, keeps track
of whether the user has the privilege to bypass user write blocking by [updating a cached variable
upon any changes to the authorization
state](https://github.com/mongodb/mongo/blob/e4032fe5c39f1974c76de4cefdc07d98ab25aeef/src/mongo/db/auth/authorization_session_impl.cpp#L1119-L1121).
This structure enables, for example, sharded writes to work correctly with user write blocking,
because the `WriteBlockBypass` state is initially set on the `mongos` based on the
`AuthorizationSession`, which knows the privileges of the user making the write request, and then
propagates from the `mongos` to the shards involved in the write. Note that this means on requests
from `mongos`, shard servers don't check their own `AuthorizationSession`s when setting
`WriteBlockBypass`. This would be incorrect behavior since internal requests have internal
authorization, which confers all privileges, including the privilege to bypass user write blocking.

The `setUserWriteBlockMode` command, before enabling user write blocking, blocks creation of new
index builds and aborts all currently running index builds on non-internal databases, and drains the
index builds it cannot abort. This upholds the invariant that while user write blocking is enabled,
all running index builds are allowed to bypass write blocking and therefore can commit without
additional checks.

In sharded clusters, enabling user write blocking is a two-phase operation, coordinated by the config
server. The first phase disallows creation of new `ShardingDDLCoordinator`s and drains all currently
running `DDLCoordinator`s. The config server waits for all shards to complete this phase before
moving onto the second phase, which aborts index builds and enables write blocking. This structure is
used because enabling write blocking while there are ongoing `ShardingDDLCoordinator`s would prevent
those operations from completing.
