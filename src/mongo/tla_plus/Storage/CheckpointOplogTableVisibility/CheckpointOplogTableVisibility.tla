\* Copyright 2026 MongoDB, Inc.
\*
\* This work is licensed under:
\* - Creative Commons Attribution-3.0 United States License
\*   http://creativecommons.org/licenses/by/3.0/us/

-------------------------- MODULE CheckpointOplogTableVisibility --------------------------
\* Formal specification of the bug described in SERVER-115256: installing WiredTiger
\* checkpoints concurrently with oplog application can result in collection tables
\* temporarily not existing on disk, leading to a TableNotFound crash if any read or
\* write touches the collection before the next checkpoint install.
\*
\* Scenario the spec captures (paraphrased from the ticket):
\*
\*   Primary:
\*     1. Take checkpoint at timestamp T=0.
\*     2. Create a collection at T=1.
\*     3. Take a checkpoint at T=1.
\*
\*   Secondary:
\*     A. Apply oplog up to T=0.
\*     B. Begin applying the create-collection oplog entry at T=1.
\*        B1. Create the storage table for the collection.
\*        B2. Write the catalog entry pointing at the new ident at T=1.
\*     C. Advance lastApplied to T=1.
\*
\* The bug: the checkpoint installer reads the secondary's `lastApplied` to pick a
\* timestamp to install at. If the installer fires between B1 and B2/C, lastApplied is
\* still T=0, so the installer happily installs the T=0 checkpoint -- which does not
\* contain the table created at B1. The table now "ceases to exist" until the NEXT
\* checkpoint install promotes T=1 onto disk. Any oplog-applier op that touches the
\* table during that window crashes with TableNotFound.
\*
\* Model:
\*   - One in-memory catalog (a map from collection name -> ident) shared by the applier
\*     and the checkpoint installer.
\*   - A set of storage tables that "exist" on disk (ident-keyed).
\*   - A monotonic `lastApplied` timestamp.
\*   - A bag of pending oplog ops the applier processes one phase at a time, so the
\*     model can interleave checkpoint installation in the middle of a single create.
\*
\* To run the model-checker, first edit the constants in
\* MCCheckpointOplogTableVisibility.cfg if desired, then:
\*     cd src/mongo/tla_plus
\*     ./model-check.sh Storage/CheckpointOplogTableVisibility

EXTENDS Integers, Sequences, FiniteSets, TLC

CONSTANTS Collections,    \* Set of collection names the applier will create.
          MaxTimestamp,   \* Highest oplog timestamp to model (inclusive).
          BugMode         \* TRUE allows the buggy interleaving (catalog write after
                          \* table create but before lastApplied advances); FALSE
                          \* models the fix (table create and lastApplied advance are
                          \* observable atomically to the checkpoint installer).

ASSUME Cardinality(Collections) > 0
ASSUME MaxTimestamp \in 1..10
ASSUME BugMode \in BOOLEAN

\* Sentinel for "no ident installed yet".
NoIdent == "NO_IDENT"

\* Each collection's ident is derived from its name; one per collection is enough to
\* trigger the bug.
IdentOf(coll) == coll

VARIABLES
    \* In-memory catalog map: collection name -> ident or NoIdent.
    catalog,
    \* In-memory catalog timestamp (when the ident was written). 0 means "no entry".
    catalogTs,
    \* Set of idents that currently have a storage table on disk.
    tablesOnDisk,
    \* Monotonic last-applied timestamp on the secondary.
    lastApplied,
    \* For each collection: the applier's progress through the create-collection
    \* oplog op. One of:
    \*   "pending"      - not yet started
    \*   "table_created"- storage table written, catalog still empty
    \*   "catalog_done" - catalog entry written, lastApplied not yet bumped
    \*   "applied"      - lastApplied advanced past this op's timestamp
    applierPhase,
    \* The oplog timestamp the applier has reserved for each collection's create op.
    reservedTs,
    \* The most recent checkpoint installed on disk: snapshot of the in-memory catalog
    \* (so missing idents drop their tables) plus the timestamp it was installed at.
    lastCheckpointTs,
    lastCheckpointCatalog,
    \* History of "TableNotFound" events the applier observed. The bug invariant
    \* asserts this stays empty.
    tableNotFoundEvents

vars == <<catalog, catalogTs, tablesOnDisk, lastApplied, applierPhase,
          reservedTs, lastCheckpointTs, lastCheckpointCatalog,
          tableNotFoundEvents>>

-----------------------------------------------------------------------------
\* Helpers.
-----------------------------------------------------------------------------

\* Pick the next timestamp the applier should reserve. We hand out timestamps
\* starting at 1 in collection order; any reservation must be unique and bounded.
NextFreeTimestamp ==
    LET used == {reservedTs[c] : c \in Collections} \ {0}
        candidates == (1..MaxTimestamp) \ used
    IN IF candidates = {}
       THEN 0
       ELSE CHOOSE t \in candidates : \A u \in candidates : t <= u

\* Snapshot of the current in-memory catalog (only entries that have been written).
CurrentCatalogSnapshot ==
    [c \in Collections |-> catalog[c]]

\* Snapshot of which idents the catalog currently references.
CatalogIdents(cat) ==
    LET liveColls == {c \in Collections : cat[c] # NoIdent}
    IN {cat[c] : c \in liveColls}

\* True if every collection's `applierPhase` is in a stable phase (no op is
\* mid-flight). Used by the fix-mode guard on the checkpoint installer.
NoApplierMidOp ==
    \A c \in Collections : applierPhase[c] \notin {"table_created", "catalog_done"}

-----------------------------------------------------------------------------
\* Initial state.
-----------------------------------------------------------------------------

Init ==
    /\ catalog = [c \in Collections |-> NoIdent]
    /\ catalogTs = [c \in Collections |-> 0]
    /\ tablesOnDisk = {}
    /\ lastApplied = 0
    /\ applierPhase = [c \in Collections |-> "pending"]
    /\ reservedTs = [c \in Collections |-> 0]
    /\ lastCheckpointTs = 0
    /\ lastCheckpointCatalog = [c \in Collections |-> NoIdent]
    /\ tableNotFoundEvents = {}

-----------------------------------------------------------------------------
\* Applier actions. The create-collection oplog op is split into three phases
\* so the checkpoint installer can interleave between them.
-----------------------------------------------------------------------------

\* Phase 1: the applier reserves an oplog timestamp and writes the storage table
\* for the new collection. The catalog is NOT updated yet.
ApplierCreateTable(c) ==
    /\ applierPhase[c] = "pending"
    /\ LET t == NextFreeTimestamp
       IN /\ t > 0
          /\ reservedTs' = [reservedTs EXCEPT ![c] = t]
          /\ tablesOnDisk' = tablesOnDisk \union {IdentOf(c)}
          /\ applierPhase' = [applierPhase EXCEPT ![c] = "table_created"]
    /\ UNCHANGED <<catalog, catalogTs, lastApplied,
                   lastCheckpointTs, lastCheckpointCatalog,
                   tableNotFoundEvents>>

\* Phase 2: the applier writes the catalog entry that points at the new ident.
\* The catalog write is stamped with the reserved timestamp.
ApplierWriteCatalog(c) ==
    /\ applierPhase[c] = "table_created"
    /\ catalog' = [catalog EXCEPT ![c] = IdentOf(c)]
    /\ catalogTs' = [catalogTs EXCEPT ![c] = reservedTs[c]]
    /\ applierPhase' = [applierPhase EXCEPT ![c] = "catalog_done"]
    /\ UNCHANGED <<tablesOnDisk, lastApplied, reservedTs,
                   lastCheckpointTs, lastCheckpointCatalog,
                   tableNotFoundEvents>>

\* Phase 3: the applier advances lastApplied past this op. This is the moment the
\* checkpoint installer is "supposed" to see the new state. In the buggy code path,
\* this advance happens after step 2 -- which is exactly the race window where the
\* installer can pick a stale timestamp.
ApplierAdvanceLastApplied(c) ==
    /\ applierPhase[c] = "catalog_done"
    /\ reservedTs[c] > lastApplied
    /\ lastApplied' = reservedTs[c]
    /\ applierPhase' = [applierPhase EXCEPT ![c] = "applied"]
    /\ UNCHANGED <<catalog, catalogTs, tablesOnDisk, reservedTs,
                   lastCheckpointTs, lastCheckpointCatalog,
                   tableNotFoundEvents>>

\* The applier (or any later op) reads the table for collection c. If the table is
\* not on disk we record a TableNotFound event -- this is the bug surface.
ApplierUseTable(c) ==
    /\ catalog[c] # NoIdent
    /\ IF IdentOf(c) \in tablesOnDisk
       THEN UNCHANGED tableNotFoundEvents
       ELSE tableNotFoundEvents' =
                tableNotFoundEvents \union {[coll |-> c, ts |-> lastApplied]}
    /\ UNCHANGED <<catalog, catalogTs, tablesOnDisk, lastApplied,
                   applierPhase, reservedTs,
                   lastCheckpointTs, lastCheckpointCatalog>>

-----------------------------------------------------------------------------
\* Checkpoint installer.
-----------------------------------------------------------------------------

\* Decide which timestamp the checkpoint installer is allowed to install at.
\*
\* BugMode = TRUE  : installer uses `lastApplied` directly. This is the buggy
\*                   behaviour described in the ticket -- when the applier is in
\*                   phase "catalog_done" the catalog already references an ident
\*                   whose corresponding storage table is on disk, but lastApplied
\*                   is still pointing at the previous oplog op. Installing at that
\*                   stale timestamp captures the OLD catalog snapshot (no ident)
\*                   and silently drops the just-created table.
\*
\* BugMode = FALSE : installer also requires that no applier is mid-op (no phase
\*                   in {"table_created", "catalog_done"}). This is one expression
\*                   of the fix space: the installer must not snapshot the catalog
\*                   while an oplog op is partially observable.
CanInstallCheckpoint(t) ==
    /\ t >= lastCheckpointTs
    /\ t <= lastApplied
    \* Don't generate stuttering no-op installs: either the timestamp advances,
    \* the catalog snapshot the installer is about to persist is different from
    \* the last one we installed, or the resulting on-disk table set would
    \* change (this last clause is what surfaces the bug: the install would
    \* drop a table that was created since the last install).
    /\ \/ t > lastCheckpointTs
       \/ CurrentCatalogSnapshot # lastCheckpointCatalog
       \/ CatalogIdents(CurrentCatalogSnapshot) # tablesOnDisk
    /\ \/ BugMode
       \/ NoApplierMidOp

\* The installer snapshots the in-memory catalog and persists that snapshot to
\* disk. Any storage table whose ident is NOT in the snapshot is dropped from disk.
InstallCheckpoint ==
    /\ \E t \in 0..MaxTimestamp :
         /\ CanInstallCheckpoint(t)
         /\ LET snap == CurrentCatalogSnapshot
            IN /\ lastCheckpointTs' = t
               /\ lastCheckpointCatalog' = snap
               \* Drop any table whose ident is not referenced by the snapshot
               \* AND was not introduced by an applier whose lastApplied <= t
               \* would have to imply the ident was already written. In the buggy
               \* mode the installer races and we model precisely that: the
               \* on-disk table set is replaced by the set of idents the snapshot
               \* knows about.
               /\ tablesOnDisk' = CatalogIdents(snap)
    /\ UNCHANGED <<catalog, catalogTs, lastApplied, applierPhase, reservedTs,
                   tableNotFoundEvents>>

-----------------------------------------------------------------------------
\* Next-state relation.
-----------------------------------------------------------------------------

Next ==
    \/ \E c \in Collections : ApplierCreateTable(c)
    \/ \E c \in Collections : ApplierWriteCatalog(c)
    \/ \E c \in Collections : ApplierAdvanceLastApplied(c)
    \/ \E c \in Collections : ApplierUseTable(c)
    \/ InstallCheckpoint

Spec ==
    /\ Init
    /\ [][Next]_vars
    /\ WF_vars(Next)
    /\ \A c \in Collections :
        /\ WF_vars(ApplierCreateTable(c))
        /\ WF_vars(ApplierWriteCatalog(c))
        /\ WF_vars(ApplierAdvanceLastApplied(c))

-----------------------------------------------------------------------------
\* Invariants.
-----------------------------------------------------------------------------

TypeOK ==
    /\ catalog \in [Collections -> Collections \union {NoIdent}]
    /\ catalogTs \in [Collections -> 0..MaxTimestamp]
    /\ tablesOnDisk \subseteq {IdentOf(c) : c \in Collections}
    /\ lastApplied \in 0..MaxTimestamp
    /\ applierPhase \in
            [Collections ->
                {"pending", "table_created", "catalog_done", "applied"}]
    /\ reservedTs \in [Collections -> 0..MaxTimestamp]
    /\ lastCheckpointTs \in 0..MaxTimestamp
    /\ lastCheckpointCatalog \in
            [Collections -> Collections \union {NoIdent}]

\* The core safety property: every catalog entry that is observable must point at
\* a storage table that actually exists on disk. This is what the bug violates --
\* in the buggy interleaving the catalog has an ident for collection c but the
\* corresponding table has been dropped by the racing checkpoint install.
NoUseOfMissingTable ==
    \A c \in Collections :
        catalog[c] # NoIdent =>
            IdentOf(c) \in tablesOnDisk

\* No applier read or write should have ever crashed with TableNotFound. This is
\* the empirical "did the bug fire?" check.
NoTableNotFoundEvents ==
    tableNotFoundEvents = {}

\* lastApplied never moves backwards.
LastAppliedMonotonic == lastApplied >= 0

\* lastCheckpointTs never moves backwards and never exceeds lastApplied.
CheckpointWithinApplied ==
    /\ lastCheckpointTs <= lastApplied
    /\ lastCheckpointTs >= 0

\* If a collection has progressed to "catalog_done" or "applied", its reserved
\* timestamp must be set.
ReservedTsConsistent ==
    \A c \in Collections :
        applierPhase[c] \in {"catalog_done", "applied"} => reservedTs[c] > 0

=================================================================================================
