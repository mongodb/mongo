# PrimaryOnlyService

The PrimaryOnlyService machinery provides a way to register tasks that should run only when current
node is Primary, and should be driven to completion across replica set failovers on the new
Primary. It is intended to be used by tasks that can be modeled as a state machine with a single
MongoDB document containing the current state, which newly-elected Primaries can use to rebuild the
state of the task after failover and pick up where the old Primary left off.

## Classes

There are three main classes/interfaces that make up the PrimaryOnlyService machinery.

### PrimaryOnlyServiceRegistry

The PrimaryOnlyServiceRegistry is a singleton that is installed as a decoration on the
ServiceContext at startup and lives for the lifetime of the mongod process.  During mongod global
startup, all PrimaryOnlyServices must be registered against the PrimaryOnlyServiceRegistry before
the ReplicationCoordinator is started up (as it is the ReplicationCoordinator startup that starts up
the registered PrimaryOnlyServices). Specific PrimaryOnlyServices can be looked up from the registry
at runtime, and are handed out by raw pointer, which is safe since the set of registered
PrimaryOnlyServices does not change during runtime.  The PrimaryOnlyServiceRegistry is itself a
[ReplicaSetAwareService](../src/mongo/db/repl/README.md#ReplicaSetAwareService-interface), which is
how it receives notifications about changes in and out of Primary state.

### PrimaryOnlyService

The PrimaryOnlyService interface is used to define a new Primary Only Service.  A PrimaryOnlyService
is a grouping of tasks (Instances) that run only when the node is Primary and are resumed after
failover.  Each PrimaryOnlyService must declare a unique, replicated collection (most likely in the
admin or config databases), where the state documents for all Instances of the service will be
persisted.  At stepUp, each PrimaryOnlyService will create and launch Instance objects for each
document found in this collection. This is how PrimaryOnlyService tasks get resumed after failover.


### PrimaryOnlyService::Instance/TypedInstance

The PrimaryOnlyService::Instance interface is used to contain the state and core logic for running a
single task belonging to a PrimaryOnlyService. The Instance interface includes a "run()" virtual
method which is provided an executor which is used to run all work that is done on behalf of the
Instance. Implementations should not extend PrimaryOnlyService::Instance directly, instead they
should extend PrimaryOnlyService::TypedInstance, which allows individual Instances to be looked up
and returned as pointers to the proper Instance sub-type. The InstanceID for an Instance is the _id
field of its state document.


## Defining a new PrimaryOnlyService

To define a new PrimaryOnlyService one must add corresponding subclasses of both PrimaryOnlyService
and PrimaryOnlyService::TypedInstance.  The PrimaryOnlyService subclass just exists to specify what
collection state documents for this service are stored in, and to hand out corresponding Instances
of the proper type.  Most of the work of a new PrimaryOnlyService will be implemented in the
PrimaryOnlyService::Instance subclass. PrimaryOnlyService::Instance subclasses will be responsible
for running the work they need to perform to complete their task, as well as for managing and
synchronizing their own in-memory and on-disk state. No part of the PrimaryOnlyService **machinery**
ever performs writes to the PrimaryOnlyService state document collections.  All writes to a given
Instance's state document (including creating it initially and deleting it when the work has been
completed) are performed by Instance implementations.  This means that for the majority of
PrimaryOnlyServices, the first step of its Instance's run() method will be to insert an initial
state document into the state document collection, to ensure that the Instance is now persisted and
will be resumed after failover.  When an Instance is resumed after failover, it is provided the
current version of the state document as it exists in the state document collection.  That document
can be used to rebuild the in-memory state for this Instance so that when run() is called it knows
what state it is in and thus what work still needs to be performed, and what work has already been
completed by the previous Primary.

To see an example bare-bones PrimaryOnlyService implementation to use as a reference, check out the
TestService defined in this unit test: https://github.com/mongodb/mongo/blob/master/src/mongo/db/repl/primary_only_service_test.cpp


## Behavior during state transitions

At stepUp, each PrimaryOnlyService queries its state document collection, and for each document
found, creates and launches a PrimaryOnlyService::Instance initialized off of the state
document. This happens asynchronously relative to the core replication stepUp process - there is no
guarantee that when stepUp completes and the RSTL lock is dropped that the PrimaryOnlyServices have
finished rebuilding all their Instances. At stepDown all Instances are interrupted, but the threads
running their work are not joined, and the Instance objects containing their in-memory state are not
released, until the next stepUp. This is done to reduce the likelihood of blocking within the state
transition process and delaying it for the entire node. This behavior does, however, guarantee that
there will never be two Instances of the same PrimaryOnlyService with the same InstanceID running at
the same time on the same node.

### Interrupting Instances at stepDown

At stepDown, there are 3 main ways that Instances are interrupted and we guarantee that no more work
is performed on behalf of any PrimaryOnlyServices.  The first is that the executor provided to each
Instance's run() method gets shut down, preventing any more work from being scheduled on behalf of
that Instance.  The second is that all OperationContexts created on threads (Clients) that are part
of an Executor owned by a PrimaryOnlyService get interrupted. The third is that each individual
Instance is explicitly interrupted, so that it can unblock any work running on threads that are
*not* a part of an executor owned by the PrimaryOnlyService that are dependent on that Instance
signaling them (e.g. commands that are waiting on the Instance to reach a certain state). Currently
this happens via a call to an interrupt() method that each Instance must override, but in the future
this is likely to change to signaling a CancellationToken owned by the Instance instead.

## Instance lifetime

Instances are held by shared_ptr in their parent PrimaryOnlyService. Each PrimaryOnlyService
releases all Instance shared_ptrs it owns on stepDown.  Additionally, a PrimaryOnlyService will
release an Instance shared_ptr when the state document for that Instance is deleted (via an
OpObserver).  Since generally speaking it is logic from an Instance's run() method that will be
responsible for deleting its state document, such logic needs to be careful as the moment the state
document is deleted, the corresponding PrimaryOnlyService is no longer keeping that Instance alive.
If an Instance has any additional logic or internal state to update after deleting its state
document, it must extend its own lifetime by capturing a shared_ptr to itself by calling
shared_from_this() before deleting its state document.
