# Concurrency Control

Theoretically, one could design a database that used only mutexes to maintain database consistency
while supporting multiple simultaneous operations; however, this solution would result in pretty bad
performance and would a be strain on the operating system. Therefore, databases typically use a more
complex method of coordinating operations. This design consists of Resources (lockable entities),
some of which may be organized in a Hierarchy, and Locks (requests for access to a resource). A Lock
Manager is responsible for keeping track of Resources and Locks, and for managing each Resource's
Lock Queue. The Lock Manager identifies Resources with a ResourceId.

## Resource Hierarchy

In MongoDB, Resources are arranged in a hierarchy, in order to provide an ordering to prevent
deadlocks when locking multiple Resources, and also as an implementation of Intent Locking (an
optimization for locking higher level resources). The hierarchy of ResourceTypes is as follows:

1. Global (three - see below)
1. Database (one per database on the server)
1. Collection (one per collection on the server)

Each resource must be locked in order from the top. Therefore, if a Collection resource is desired
to be locked, one must first lock the one Global resource, and then lock the Database resource that
is the parent of the Collection. Finally, the Collection resource is locked.

In addition to these ResourceTypes, there also exists ResourceMutex, which is independent of this
hierarchy. One can use ResourceMutex instead of a regular mutex if one desires the features of the
lock manager, such as fair queuing and the ability to have multiple simultaneous lock holders.

## Lock Modes

The lock manager keeps track of each Resource's _granted locks_ and a queue of _waiting locks_.
Rather than the binary "locked-or-not" modes of a mutex, a MongoDB lock can have one of several
_modes_. Modes have different _compatibilities_ with other locks for the same resource. Locks with
compatible modes can be simultaneously granted to the same resource, while locks with modes that are
incompatible with any currently granted lock on a resource must wait in the waiting queue for that
resource until the conflicting granted locks are unlocked. The different types of modes are:

1. X (exclusive): Used to perform writes and reads on the resource.
2. S (shared): Used to perform only reads on the resource (thus, it is okay to Share with other
   compatible locks).
3. IX (intent-exclusive): Used to indicate that an X lock is taken at a level in the hierarchy below
   this resource. This lock mode is used to block X or S locks on this resource.
4. IS (intent-shared): Used to indicate that an S lock is taken at a level in the hierarchy below
   this resource. This lock mode is used to block X locks on this resource.

## Lock Compatibility Matrix

This matrix answers the question, given a granted lock on a resource with the mode given, is a
requested lock on that same resource with the given mode compatible?

| Requested Mode |           |         | Granted Mode |        |        |
| :------------- | :-------: | :-----: | :----------: | :----: | :----: |
|                | MODE_NONE | MODE_IS |   MODE_IX    | MODE_S | MODE_X |
| MODE_IS        |     Y     |    Y    |      Y       |   Y    |   N    |
| MODE_IX        |     Y     |    Y    |      Y       |   N    |   N    |
| MODE_S         |     Y     |    Y    |      N       |   Y    |   N    |
| MODE_X         |     Y     |    N    |      N       |   N    |   N    |

Typically, locks are granted in the order they are queued, but some LockRequest behaviors can be
specially selected to break this rule. One behavior is _enqueueAtFront_, which allows important lock
acquisitions to cut to the front of the line, in order to expedite them. Currently, all mode X and S
locks for the three Global Resources (Global, MultiDocumentTransactionsBarrier, RSTL) automatically
use this option.
Another behavior is _compatibleFirst_, which allows compatible lock requests to cut ahead of others
waiting in the queue and be granted immediately; note that this mode might starve queued lock
requests indefinitely.

### Replication State Transition Lock (RSTL)

The Replication State Transition Lock is of ResourceType Global, so it must be locked prior to
locking any Database level resource. This lock is used to synchronize replica state transitions
(typically transitions between PRIMARY, SECONDARY, and ROLLBACK states).
More information on the RSTL is contained in the [Replication Architecture Guide](https://github.com/mongodb/mongo/blob/b4db8c01a13fd70997a05857be17548b0adec020/src/mongo/db/repl/README.md#replication-state-transition-lock)

### Global Lock

The resource known as the Global Lock is of ResourceType Global. It is currently used to
synchronize shutdown, so that all operations are finished with the storage engine before closing it.
Certain types of global storage engine operations, such as recoverToStableTimestamp(), also require
this lock to be held in exclusive mode.

### Tenant Lock

A resource of ResourceType Tenant is used when a database belongs to a tenant. It is used to synchronize
change streams enablement and disablement for a tenant operation with other operations associated with the tenant.
Enabling or disabling of change streams (by creating or dropping a change collection) for a tenant takes this lock
in exclusive (X) mode. Acquiring this resource with an intent lock is an indication that the operation is doing reads (IS)
or writes (IX) at the database or lower level.

### Database Lock

Any resource of ResourceType Database protects certain database-wide operations such as database
drop. These operations are being phased out, in the hopes that we can eliminate this ResourceType
completely.

### Collection Lock

Any resource of ResourceType Collection protects certain collection-wide operations, and in some
cases also protects the in-memory catalog structure consistency in the face of concurrent readers
and writers of the catalog. Acquiring this resource with an intent lock is an indication that the
operation is doing explicit reads (IS) or writes (IX) at the document level. There is no Document
ResourceType, as locking at this level is done in the storage engine itself for efficiency reasons.

### Document Level Concurrency Control

Each storage engine is responsible for locking at the document level. The [WiredTiger storage
engine](../storage/wiredtiger/README.md) uses MVCC [multi-version concurrency control][Multiversion concurrency control] along with optimistic locking in order to provide concurrency guarantees.

## Two-Phase Locking

The lock manager automatically provides _two-phase locking_ for a given storage transaction.
Two-phase locking consists of an Expanding phase where locks are acquired but not released, and a
subsequent Shrinking phase where locks are released but not acquired. By adhering to this protocol,
a transaction will be guaranteed to be serializable with other concurrent transactions. The
WriteUnitOfWork class manages two-phase locking in MongoDB. This results in the somewhat unexpected
behavior of the RAII locking types acquiring locks on resources upon their construction but not
unlocking the lock upon their destruction when going out of scope. Instead, the responsibility of
unlocking the locks is transferred to the WriteUnitOfWork destructor. Note this is only true for
transactions that do writes, and therefore only for code that uses WriteUnitOfWork.
