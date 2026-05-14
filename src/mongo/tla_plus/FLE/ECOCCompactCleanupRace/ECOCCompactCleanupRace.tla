\* Copyright 2026 MongoDB, Inc.
\*
\* This work is licensed under:
\* - Creative Commons Attribution-3.0 United States License
\*   http://creativecommons.org/licenses/by/3.0/us/

------------------------------ MODULE ECOCCompactCleanupRace --------------------------------------
\* Formal specification of the race between FLE2 queryable-encryption (QE) cleanup/compact
\* operations and concurrent user writes against an encrypted collection.
\*
\* Reference: SERVER-126384 — "QE cleanup/compact racing with user writes might result in ECOC
\* collection missing the clustered index".
\*
\* Real-world setup (paraphrased from the ticket):
\*   The `compactStructuredEncryptionData` and `cleanupStructuredEncryptionData` commands operate
\*   on the per-collection ECOC (encrypted compaction collection). Both commands work by:
\*     (1) Renaming the live `enxcol_.<name>.ecoc` collection out of the way (to `.ecoc.compact`
\*         or `.ecoc.cleanup`).
\*     (2) Releasing the collection lock between rename and create.
\*     (3) Re-creating an empty `enxcol_.<name>.ecoc` WITH the required clustered index option
\*         (`{clusteredIndex: {key: {_id: 1}, unique: true}}`).
\*
\*   Step (2) opens a window in which a concurrent user insert/update on the encrypted data
\*   collection (EDC) may implicitly create the ECOC namespace using DEFAULT collection options
\*   — i.e. WITHOUT the clustered index. When this happens, the internal `create` issued by the
\*   maintenance command silently no-ops (collection already exists) and the command returns
\*   success to the caller. The result is a long-lived ECOC missing the clustered index, with
\*   permanent performance and correctness consequences for subsequent compaction.
\*
\* This specification:
\*   - Models the ECOC namespace as a record describing its existence and creation-time options.
\*   - Models compact, cleanup, and concurrent user-write actions as distinct steps so the race
\*     interleaving (rename -> implicit-create -> internal-create) is visible to TLC.
\*   - Pins the safety invariant ECOCAlwaysClustered: while it exists, the ECOC namespace
\*     ALWAYS carries the clustered index option.
\*   - Models the proposed catalog-level fix (Last Comment by Pierlauro Sciarelli): the constant
\*     `CatalogPinsClusteredIndex`, when TRUE, forces every implicit ECOC creation to use the
\*     clustered-index option. With this constant FALSE the spec exhibits the bug; with it TRUE
\*     the bug is masked. Both modes are exercised by the model-check config so the constant
\*     functions as a regression switch.
\*
\* To run the model-checker, first edit the constants in MCECOCCompactCleanupRace.cfg if desired,
\* then:
\*     cd src/mongo/tla_plus
\*     ./model-check.sh FLE/ECOCCompactCleanupRace

EXTENDS Integers, Sequences, FiniteSets, TLC

CONSTANTS
    UserWriters,                 \* Set of concurrent user-write thread ids.
    MaxMaintenanceOps,           \* Bound on (compact + cleanup) ops to keep state finite.
    MaxUserWrites,               \* Bound on user-write ops to keep state finite.
    CatalogPinsClusteredIndex    \* Proposed fix: catalog forces clustered index on every
                                 \* implicit ECOC create. BOOLEAN.

\* Maintenance commands. cleanup and compact follow the same rename->release->create pattern
\* against the ECOC namespace; we model them as two separate kinds so the spec can show both
\* paths exhibit the race.
COMPACT == "compact"
CLEANUP == "cleanup"
MaintenanceKinds == {COMPACT, CLEANUP}

\* ECOC namespace creation provenance, used to make the buggy interleaving observable.
CREATED_BY_MAINTENANCE == "maintenance"   \* Created by compactor/cleaner with clustered index.
CREATED_BY_USER_WRITE  == "user_write"    \* Implicitly created by a user insert/update.

\* Phases a maintenance op walks through. RENAMED is the dangerous gap: the original ECOC has
\* been renamed but a new one has not yet been re-created.
IDLE     == "idle"
RENAMED  == "renamed"
CREATED  == "created"
DONE     == "done"

MaintenancePhases == {IDLE, RENAMED, CREATED, DONE}

ASSUME Cardinality(UserWriters) > 0
ASSUME MaxMaintenanceOps \in 1..100
ASSUME MaxUserWrites \in 1..100
ASSUME CatalogPinsClusteredIndex \in BOOLEAN

-----------------------------------------------------------------------------

VARIABLES
    ecocExists,        \* BOOLEAN: does the live ECOC namespace currently exist?
    ecocClustered,     \* BOOLEAN: if ecocExists, does it have the clustered index option?
    ecocCreatedBy,     \* Provenance of the current ECOC: maintenance or user_write.
    ecocTempExists,    \* BOOLEAN: does the renamed-out temp namespace currently exist?
    maintenancePhase,  \* IDLE | RENAMED | CREATED | DONE (current op state).
    maintenanceKind,   \* COMPACT | CLEANUP (kind of the in-flight op, if any).
    maintenanceCount,  \* Number of maintenance ops started (for bounding).
    userWriteCount,    \* Number of user writes performed (for bounding).
    history            \* Sequence of observed events, helpful for counterexample readability.

vars == <<ecocExists, ecocClustered, ecocCreatedBy, ecocTempExists,
          maintenancePhase, maintenanceKind, maintenanceCount, userWriteCount, history>>

-----------------------------------------------------------------------------

\* Initial state: the encrypted collection has just been created. ECOC exists with the
\* clustered index option, provenance is "maintenance" (the create-encrypted-collection path
\* installs it). No maintenance op in flight, no user writes yet.
Init ==
    /\ ecocExists = TRUE
    /\ ecocClustered = TRUE
    /\ ecocCreatedBy = CREATED_BY_MAINTENANCE
    /\ ecocTempExists = FALSE
    /\ maintenancePhase = IDLE
    /\ maintenanceKind = COMPACT  \* Arbitrary placeholder; only meaningful while phase # IDLE.
    /\ maintenanceCount = 0
    /\ userWriteCount = 0
    /\ history = <<>>

\* Convenience operator: record an event for counterexample traces.
AppendHistory(e) == history' = Append(history, e)

-----------------------------------------------------------------------------
\* Maintenance command actions (compact or cleanup).
-----------------------------------------------------------------------------

\* Step 1: start a maintenance op. Atomically rename the current ECOC out of the way under the
\* collection lock. After this step the ECOC namespace is gone and we are in the RENAMED phase.
\* This is the model of:
\*     RenameCollection(enxcol_.<n>.ecoc -> enxcol_.<n>.ecoc.<kind>)  // holds coll lock
StartMaintenance(kind) ==
    /\ kind \in MaintenanceKinds
    /\ maintenancePhase = IDLE
    /\ maintenanceCount < MaxMaintenanceOps
    /\ ecocExists                               \* Cannot rename a non-existent ECOC.
    /\ ~ecocTempExists                          \* No previous temp lying around.
    /\ ecocExists' = FALSE                      \* Renamed out: live ECOC namespace gone.
    /\ ecocClustered' = FALSE                   \* (Don't-care while ecocExists' = FALSE.)
    /\ ecocTempExists' = TRUE
    /\ maintenancePhase' = RENAMED
    /\ maintenanceKind' = kind
    /\ maintenanceCount' = maintenanceCount + 1
    /\ UNCHANGED <<ecocCreatedBy, userWriteCount>>
    /\ AppendHistory(<<"maintenance_renamed", kind>>)

\* Step 2: process the renamed-out ECOC (delete it / fold its tokens into ESC). This step does
\* not touch the live ECOC namespace; we simply mark the temp as gone. It exists primarily so
\* the spec can demonstrate that user writes happening BEFORE the internal create is what
\* triggers the bug, regardless of how long this internal processing takes. May fire in either
\* RENAMED or CREATED phase (real code drops the temp after consuming tokens, which can be
\* before or after the internal create depending on cleanup vs. compact ordering).
ProcessRenamedECOC ==
    /\ maintenancePhase \in {RENAMED, CREATED}
    /\ ecocTempExists
    /\ ecocTempExists' = FALSE
    /\ UNCHANGED <<ecocExists, ecocClustered, ecocCreatedBy,
                   maintenancePhase, maintenanceKind, maintenanceCount, userWriteCount>>
    /\ AppendHistory(<<"maintenance_processed_temp", maintenanceKind>>)

\* Step 3: the maintenance op tries to recreate the ECOC with the clustered index option. Two
\* outcomes mirror the real code path in fle2_compact_cmd.cpp / fle2_cleanup_cmd.cpp:
\*   (a) The collection does NOT already exist: the create succeeds, installs the clustered
\*       index, and we transition to CREATED with provenance = maintenance.
\*   (b) The collection DOES already exist (a concurrent user write implicitly created it
\*       between RENAMED and here): the create call silently no-ops. The existing namespace
\*       — created without the clustered index — survives.
MaintenanceCreateECOC ==
    /\ maintenancePhase = RENAMED
    /\ \/ /\ ~ecocExists                        \* (a) Happy path.
          /\ ecocExists' = TRUE
          /\ ecocClustered' = TRUE
          /\ ecocCreatedBy' = CREATED_BY_MAINTENANCE
          /\ AppendHistory(<<"maintenance_create_ok", maintenanceKind>>)
       \/ /\ ecocExists                         \* (b) Silent no-op; bug surface.
          /\ UNCHANGED <<ecocExists, ecocClustered, ecocCreatedBy>>
          /\ AppendHistory(<<"maintenance_create_noop_collection_exists", maintenanceKind>>)
    /\ maintenancePhase' = CREATED
    /\ UNCHANGED <<ecocTempExists, maintenanceKind, maintenanceCount, userWriteCount>>

\* Step 4: maintenance command returns success to the user. Requires the renamed-out temp
\* namespace to have already been processed and dropped, mirroring the actual command which
\* will not return until its compactor has consumed the temp's tokens.
MaintenanceFinish ==
    /\ maintenancePhase = CREATED
    /\ ~ecocTempExists
    /\ maintenancePhase' = IDLE
    /\ UNCHANGED <<ecocExists, ecocClustered, ecocCreatedBy, ecocTempExists,
                   maintenanceKind, maintenanceCount, userWriteCount>>
    /\ AppendHistory(<<"maintenance_done", maintenanceKind>>)

-----------------------------------------------------------------------------
\* User write action.
-----------------------------------------------------------------------------

\* A concurrent user insert/update on the EDC. When the writer reaches the inner FLE write path
\* it must touch the ECOC. If the ECOC namespace does not currently exist, MongoDB will
\* IMPLICITLY create it. There are two sub-cases:
\*   - With the proposed catalog-level fix (CatalogPinsClusteredIndex = TRUE): the implicit
\*     create uses {clusteredIndex: {key: {_id: 1}, unique: true}}. Bug masked.
\*   - Without the fix (CatalogPinsClusteredIndex = FALSE): the implicit create uses DEFAULT
\*     options. ECOC ends up without its clustered index. Bug reproduces.
\* If the ECOC namespace already exists, the user write simply targets it; no creation happens
\* and the clustered-index property is unchanged.
UserWrite(w) ==
    /\ w \in UserWriters
    /\ userWriteCount < MaxUserWrites
    /\ IF ecocExists
       THEN /\ UNCHANGED <<ecocExists, ecocClustered, ecocCreatedBy>>
            /\ AppendHistory(<<"user_write_to_existing_ecoc", w>>)
       ELSE /\ ecocExists' = TRUE
            /\ ecocClustered' = CatalogPinsClusteredIndex
            /\ ecocCreatedBy' = CREATED_BY_USER_WRITE
            /\ AppendHistory(<<"user_write_implicit_create_ecoc",
                                w, CatalogPinsClusteredIndex>>)
    /\ userWriteCount' = userWriteCount + 1
    /\ UNCHANGED <<ecocTempExists, maintenancePhase, maintenanceKind, maintenanceCount>>

-----------------------------------------------------------------------------

Next ==
    \/ \E k \in MaintenanceKinds : StartMaintenance(k)
    \/ ProcessRenamedECOC
    \/ MaintenanceCreateECOC
    \/ MaintenanceFinish
    \/ \E w \in UserWriters : UserWrite(w)

\* Allow infinite stuttering once both bounds are hit.
Done ==
    /\ maintenanceCount = MaxMaintenanceOps
    /\ userWriteCount = MaxUserWrites
    /\ maintenancePhase = IDLE

Spec == /\ Init
        /\ [][Next \/ (Done /\ UNCHANGED vars)]_vars
        /\ WF_vars(ProcessRenamedECOC)
        /\ WF_vars(MaintenanceCreateECOC)
        /\ WF_vars(MaintenanceFinish)

-----------------------------------------------------------------------------
\* Invariants
-----------------------------------------------------------------------------

TypeOK ==
    /\ ecocExists \in BOOLEAN
    /\ ecocClustered \in BOOLEAN
    /\ ecocCreatedBy \in {CREATED_BY_MAINTENANCE, CREATED_BY_USER_WRITE}
    /\ ecocTempExists \in BOOLEAN
    /\ maintenancePhase \in MaintenancePhases
    /\ maintenanceKind \in MaintenanceKinds
    /\ maintenanceCount \in 0..MaxMaintenanceOps
    /\ userWriteCount \in 0..MaxUserWrites
    \* history is a Seq of event tuples whose first element is a string label; we deliberately
    \* avoid an explicit Seq(STRING) constraint because TLC cannot enumerate STRING. The bound
    \* below matches the worst case: 4 events per maintenance op (rename, process_temp, create,
    \* done) plus 1 event per user write.
    /\ Len(history) \in 0..(4 * MaxMaintenanceOps + MaxUserWrites)

\* The core safety invariant from SERVER-126384: the ECOC namespace, whenever it exists, MUST
\* carry the clustered index. (It is permissible for ECOC to not exist — e.g. mid-rename — but
\* once it exists it must be clustered.)
\*
\* Without the catalog-level fix, this invariant is violated by the interleaving:
\*   StartMaintenance(compact)            // ecocExists = FALSE
\*   UserWrite(w)                         // ecocExists = TRUE, ecocClustered = FALSE
\*   MaintenanceCreateECOC (no-op branch) // ecocExists stays TRUE, ecocClustered stays FALSE
\*   MaintenanceFinish
\* With CatalogPinsClusteredIndex = TRUE the user-write branch sets ecocClustered = TRUE, and
\* the invariant holds for every reachable state.
ECOCAlwaysClustered ==
    ecocExists => ecocClustered

\* While a maintenance op is in flight, exactly one of {live ECOC, temp ECOC} is "the ECOC
\* token store" in the catalog (and during the RENAMED phase only the temp exists, until the
\* user write implicitly recreates the live one).
ECOCExistenceConsistent ==
    /\ maintenancePhase = IDLE => ~ecocTempExists
    /\ maintenancePhase = CREATED => ~ecocTempExists \/ ecocExists

\* The maintenance op cannot accidentally end with no ECOC at all (would break subsequent FLE
\* writes outright). After Finish the ECOC must exist.
ECOCExistsAfterMaintenance ==
    maintenancePhase = IDLE => ecocExists

-----------------------------------------------------------------------------
\* Liveness
-----------------------------------------------------------------------------

\* Every started maintenance op eventually returns to IDLE.
MaintenanceEventuallyFinishes ==
    maintenancePhase # IDLE ~> maintenancePhase = IDLE

\* The ECOC namespace is eventually present and clustered (under the fix).
ECOCEventuallyClustered ==
    <>[](ecocExists /\ ecocClustered)

===================================================================================================
