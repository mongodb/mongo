\* Copyright 2026 MongoDB, Inc.
\*
\* This work is licensed under:
\* - Creative Commons Attribution-3.0 United States License
\*   http://creativecommons.org/licenses/by/3.0/us/

--------------------------- MODULE DefaultReadConcernAfterClusterTime ---------------------------
\* SERVER-126299: Cluster-wide default read concern is not honored when
\* afterClusterTime is sent without an explicit level.
\*
\* When a cluster has setDefaultRWConcern{ defaultReadConcern: {level: "majority"} },
\* a client request of the shape { readConcern: { afterClusterTime: T } }
\* (with "level" omitted, as a causally-consistent session does) must resolve
\* the effective level to the cluster-wide default ("majority"), NOT to "local".
\*
\* If the level resolves to "local", the read can return a value at a cluster
\* time T whose corresponding oplog entry has not yet been majority-committed,
\* violating the contract the operator established with setDefaultRWConcern.
\*
\* This spec models the read-concern resolution as a pure function of:
\*   (clientLevel, clientHasAfterClusterTime, clusterDefaultLevel)
\* and an effective oplog-visibility function over the resolved level.
\*
\* To model-check:
\*     cd src/mongo/tla_plus
\*     ./model-check.sh Replication/DefaultReadConcernAfterClusterTime

EXTENDS Integers, FiniteSets, Sequences, TLC

\* "majority" or "local". The cluster-wide default read concern level.
CONSTANT ClusterDefaultLevel

\* TRUE  = patched (fixed) behavior: an afterClusterTime-only request
\*         resolves its level from the cluster default.
\* FALSE = buggy behavior (pre-SERVER-126299 fix): an afterClusterTime-only
\*         request resolves to "local" regardless of the cluster default.
CONSTANT ResolveLevelFromDefault

\* Bound on the number of oplog entries the primary can issue.
CONSTANT MaxOpTime

----
\* Levels and shapes used by the client.
Levels == {"local", "majority"}

\* What a client may send. A client either:
\*   - omits readConcern entirely               (sends no level, no aCT)
\*   - sends an explicit level only             (no aCT)
\*   - sends afterClusterTime only              (level omitted)  <-- the bug case
\*   - sends both an explicit level and aCT
ClientShapes == {"none", "levelOnly", "actOnly", "levelAndAct"}

----
\* Variables describing the primary's view of the oplog.

\* Sequence of oplog timestamps the primary has issued. Each entry is an Int
\* (the cluster time of the write). Strictly increasing.
VARIABLE oplog

\* The largest cluster time that has been majority-acknowledged.
\* committedClusterTime <= last entry in oplog.
VARIABLE committedClusterTime

\* The set of read requests issued so far. Each request is a record:
\*   [ clientShape, clientLevel, afterClusterTime, resolvedLevel,
\*     waited, observedClusterTime ]
\* "waited" is TRUE iff the resolved level forced the server to block until
\* the requested afterClusterTime was majority-committed.
\* "observedClusterTime" is the cluster time of the entry returned to the
\* client (or 0 for a "none" request reading from before any write).
VARIABLE reads

vars == <<oplog, committedClusterTime, reads>>

----
\* Helpers.

\* The latest cluster time the primary has ever issued.
LatestOpTime == IF Len(oplog) = 0 THEN 0 ELSE oplog[Len(oplog)]

\* Read-concern resolution. Returns the effective level the server uses.
\* The bug is exactly the "actOnly" arm.
ResolveLevel(clientShape, clientLevel) ==
    CASE clientShape = "none"        -> ClusterDefaultLevel
      [] clientShape = "levelOnly"   -> clientLevel
      [] clientShape = "actOnly"     -> IF ResolveLevelFromDefault
                                        THEN ClusterDefaultLevel
                                        ELSE "local"
      [] clientShape = "levelAndAct" -> clientLevel

\* Given a resolved level and an afterClusterTime (0 means "no aCT"), is
\* cluster time t visible right now?
\*   - level "local"    : any t in the primary oplog is visible.
\*   - level "majority" : only t <= committedClusterTime is visible.
\* aCT >0 additionally requires the server to wait until aCT is satisfied
\* under the resolved level (i.e., aCT entry is visible under that level).
LevelVisible(level, t) ==
    \/ level = "local"     /\ t \in {oplog[i] : i \in 1..Len(oplog)} \cup {0}
    \/ level = "majority"  /\ t <= committedClusterTime

\* The server may only return an entry that is BOTH:
\*   (a) visible under the resolved level, AND
\*   (b) at least as recent as the requested afterClusterTime (when given).
Returnable(level, aCT, t) ==
    /\ t >= aCT
    /\ LevelVisible(level, t)
    /\ \/ aCT = 0
       \/ LevelVisible(level, aCT)

----
\* Initial state.

Init ==
    /\ oplog = <<>>
    /\ committedClusterTime = 0
    /\ reads = {}

----
\* Actions.

\* The primary issues a new write at the next cluster time.
PrimaryWrite ==
    /\ Len(oplog) < MaxOpTime
    /\ oplog' = Append(oplog, LatestOpTime + 1)
    /\ UNCHANGED <<committedClusterTime, reads>>

\* A majority of secondaries replicate a previously-issued entry, advancing
\* committedClusterTime. We allow the commit point to advance to any oplog
\* entry already issued, monotonically.
AdvanceCommitPoint ==
    \E t \in {oplog[i] : i \in 1..Len(oplog)} :
        /\ t > committedClusterTime
        /\ committedClusterTime' = t
        /\ UNCHANGED <<oplog, reads>>

\* A client issues a read request and the server resolves + serves it.
\* The "actOnly" case (afterClusterTime without level) is the one
\* SERVER-126299 mishandles.
ClientRead ==
    \E shape \in ClientShapes,
       lvl   \in Levels,
       aCT   \in 0..LatestOpTime,
       t     \in {oplog[i] : i \in 1..Len(oplog)} \cup {0} :
        LET rlvl == ResolveLevel(shape, lvl) IN
        /\ \* "actOnly" / "levelAndAct" must have aCT > 0; the level-only and
           \* none shapes must not carry an aCT.
           CASE shape = "none"        -> aCT = 0
             [] shape = "levelOnly"   -> aCT = 0
             [] shape = "actOnly"     -> aCT > 0
             [] shape = "levelAndAct" -> aCT > 0
        /\ Returnable(rlvl, aCT, t)
        /\ reads' = reads \cup {[
               clientShape         |-> shape,
               clientLevel         |-> lvl,
               afterClusterTime    |-> aCT,
               resolvedLevel       |-> rlvl,
               observedClusterTime |-> t
           ]}
        /\ UNCHANGED <<oplog, committedClusterTime>>

Next ==
    \/ PrimaryWrite
    \/ AdvanceCommitPoint
    \/ ClientRead

Spec == Init /\ [][Next]_vars

----
\* Invariants.

\* No two oplog entries share a cluster time, and timestamps are increasing.
OplogStrictlyMonotonic ==
    \A i, j \in 1..Len(oplog) : i < j => oplog[i] < oplog[j]

\* committedClusterTime never exceeds the largest oplog entry.
CommitPointBoundedByOplog == committedClusterTime <= LatestOpTime

\* THE CORRECTNESS PROPERTY OF INTEREST.
\* When the operator has set the cluster-wide default to "majority", any
\* read that did NOT explicitly request a different level must observe
\* only majority-committed data. Clients that explicitly send
\* {level: "local"} (i.e. "levelOnly" or "levelAndAct" with level=local)
\* have opted out of the default and are intentionally excluded.
\*
\* The "actOnly" shape (afterClusterTime, no level) is the bug case:
\* on the pre-fix path it silently resolves to "local" even though the
\* client never asked for "local".
HonorClusterDefaultMajority ==
    ClusterDefaultLevel = "majority" =>
        \A r \in reads :
            (r.clientShape \in {"none", "actOnly"}) =>
                \/ r.observedClusterTime <= committedClusterTime
                \/ r.observedClusterTime = 0

\* A weaker witness predicate that names the buggy shape explicitly: under
\* the fix, an aCT-only request must resolve to the cluster default level.
ActOnlyResolvesToDefault ==
    ResolveLevelFromDefault =>
        \A r \in reads :
            r.clientShape = "actOnly" =>
                r.resolvedLevel = ClusterDefaultLevel

=============================================================================
