\* Copyright 2026 MongoDB, Inc.
\*
\* This work is licensed under:
\* - Creative Commons Attribution-3.0 United States License
\*   http://creativecommons.org/licenses/by/3.0/us/
-------------------------------- MODULE UnshardChunklessCSS ----------------------------------------
\* Formal specification of the per-shard CollectionShardingState (CSS) UUID consistency property
\* targeted by SERVER-123346:
\*
\*     UnshardCollection leaves stale metadata in CSS on chunkless shards.
\*
\* Context (from the ticket): unshardCollection is implemented on top of resharding. The reshard
\* commit path installs the new (post-unshard) collection on the toShard and updates the config
\* server's global catalog with the new UUID. Shards that previously owned chunks but no longer do
\* (chunkless shards) are NOT visited by the commit step that refreshes their CSS, so their CSS
\* keeps pointing at the pre-reshard UUID. Today the divergence is benign because the next chunk
\* arrival sanitizes the CSS; once dropCollection becomes shard-catalog-authoritative
\* (the dropCollection work that surfaced this), it tassert()s that every targeted shard's CSS UUID
\* matches the config server's UUID and crashes on the chunkless shard.
\*
\* The spec models the unshardCollection participant graph as four roles:
\*   - configServer (global catalog: ns -> UUID)
\*   - donorShard   (had chunks before reshard, may or may not be toShard)
\*   - chunklessShard (had chunks before reshard, does NOT own any chunks after)
\*   - recipientShard (== toShard: owns the single post-unshard chunk)
\*
\* The model tracks per-shard CSS entries with two fields: `uuid` and `ownsChunks`. The reshard
\* commit is modelled as a multi-step protocol that bumps the config server UUID, then refreshes
\* the toShard's CSS, leaving the chunkless shard's CSS untouched (the bug). A simulated downstream
\* dropCollection then walks every shard and reads `css.uuid`; the safety property
\* `CSSUUIDMatchesConfigServerOrIsCleared` asserts that on every shard whose CSS is populated, the
\* UUID equals the config server's. The bug is reproduced as a violation of this invariant on the
\* chunkless shard at the moment of the post-unshard dropCollection scan.
\*
\* The fix being explored on SPM-3961 (commit-shard-authoritative resharding) is modelled by an
\* optional action `RefreshChunklessShardCSS` that, when enabled, runs as part of reshard commit
\* and brings the chunkless shard's CSS UUID in line with the config server. With the fix enabled,
\* `CSSUUIDMatchesConfigServerOrIsCleared` holds; with the fix disabled, TLC produces a
\* counterexample identical to the manual reproduction in
\* unshard_collection_stale_css_on_chunkless_shard.js.
\*
\* To run the model-checker, first edit the constants in MCUnshardChunklessCSS.cfg if desired, then:
\*     cd src/mongo/tla_plus
\*     ./model-check.sh Sharding/UnshardChunklessCSS

EXTENDS Integers, Sequences, FiniteSets, TLC

CONSTANTS
    Shards,           \* Set of shard ids participating in the cluster.
    NameSpaces,       \* Set of namespaces. Only one will be unshardCollection-ed per behavior.
    UNSHARDS,         \* Cap on number of unshardCollection commits explored.
    FIX_ENABLED       \* BOOLEAN: model SPM-3961 fix (refresh chunkless shard CSS at commit).

NoUUID == 0          \* Sentinel for "no CSS entry for this ns on this shard".

\* Phases of an unshardCollection (resharding-backed) commit on a given namespace.
PHASE_IDLE     == "idle"      \* No unshard in flight; ns is in steady state.
PHASE_CLONE    == "clone"     \* Recipient is cloning data, config UUID not yet bumped.
PHASE_COMMIT   == "commit"    \* Config UUID just bumped; CSS refresh in progress.
PHASE_DONE     == "done"      \* Commit finished; subsequent operations can target the ns.
Phases == { PHASE_IDLE, PHASE_CLONE, PHASE_COMMIT, PHASE_DONE }

\* DropCollection (the consumer that exposed the bug) is modelled as a scan + check pass.
DROP_IDLE      == "drop_idle"
DROP_SCANNING  == "drop_scanning"
DROP_FAILED    == "drop_failed"   \* tassert fired: CSS UUID mismatch on some shard.
DROP_OK        == "drop_ok"       \* every CSS entry matched the config server's UUID.
DropPhases == { DROP_IDLE, DROP_SCANNING, DROP_FAILED, DROP_OK }

ASSUME Cardinality(Shards) >= 3      \* need >=1 chunkless, 1 recipient, 1 config-equivalent peer.
ASSUME Cardinality(NameSpaces) >= 1
ASSUME UNSHARDS \in 1..10
ASSUME FIX_ENABLED \in BOOLEAN

(**************************************************************************************************)
(* Variables.                                                                                     *)
(**************************************************************************************************)

\* Global / config-server authoritative catalog.
VARIABLE configUUID         \* configUUID[ns] = UUID at the global catalog (config.collections).
VARIABLE nextUUID           \* Monotone counter used to mint fresh UUIDs.
VARIABLE unshardsRemaining  \* Decrementing budget to bound state-space exploration.

\* Resharding / unshardCollection coordinator state (lives at the config server in reality).
VARIABLE phase              \* phase[ns] \in Phases.
VARIABLE recipientOf        \* recipientOf[ns] = toShard for the in-flight unshard (or chosen value
                            \* for a completed one).
VARIABLE chunklessOf        \* chunklessOf[ns] = set of shards that lost their chunks in the
                            \* in-flight unshard. These are the shards exhibiting the bug.

\* Per-shard CollectionShardingState. Each entry is [uuid |-> Nat, ownsChunks |-> BOOLEAN].
\* A missing entry (uuid = NoUUID, ownsChunks = FALSE) means the shard has no opinion on the ns.
VARIABLE css

\* Downstream dropCollection state machine (the bug's victim).
VARIABLE dropPhase          \* dropPhase[ns] \in DropPhases.
VARIABLE dropMismatchOn     \* dropMismatchOn[ns] = shard that caused the tassert, or "none".

vars == << configUUID, nextUUID, unshardsRemaining, phase, recipientOf, chunklessOf,
           css, dropPhase, dropMismatchOn >>

(**************************************************************************************************)
(* Helpers.                                                                                       *)
(**************************************************************************************************)

CSSEntryFormat == [uuid: Nat, ownsChunks: BOOLEAN]
EmptyCSSEntry == [uuid |-> NoUUID, ownsChunks |-> FALSE]

\* Is the CSS entry "populated" enough to be checked by the downstream tassert?
\* In the real system, a chunkless shard still carries shard-key + UUID even after losing all
\* chunks; that's exactly the leftover state the ticket describes.
CSSPopulated(s, ns) == css[s][ns].uuid # NoUUID

(**************************************************************************************************)
(* Init.                                                                                          *)
(**************************************************************************************************)

Init ==
    \* Seed every ns as sharded with a fresh UUID, owned across all shards (typical pre-unshard
    \* world). This is the minimum starting state that lets the chunkless-shard scenario emerge.
    /\ configUUID = [ns \in NameSpaces |-> 1]
    /\ nextUUID = 2
    /\ unshardsRemaining = UNSHARDS
    /\ phase = [ns \in NameSpaces |-> PHASE_IDLE]
    /\ recipientOf = [ns \in NameSpaces |-> CHOOSE s \in Shards : TRUE]
    /\ chunklessOf = [ns \in NameSpaces |-> {}]
    /\ css = [s \in Shards |-> [ns \in NameSpaces |->
                [uuid |-> 1, ownsChunks |-> TRUE]]]
    /\ dropPhase = [ns \in NameSpaces |-> DROP_IDLE]
    /\ dropMismatchOn = [ns \in NameSpaces |-> "none"]

(**************************************************************************************************)
(* Actions.                                                                                       *)
(**************************************************************************************************)

\* Start unshardCollection(ns) with the recipient = toShard. Marks every other shard that had
\* chunks as chunkless for this unshard.
StartUnshard(ns, toShard) ==
    /\ unshardsRemaining > 0
    /\ phase[ns] = PHASE_IDLE
    /\ toShard \in Shards
    /\ phase' = [phase EXCEPT ![ns] = PHASE_CLONE]
    /\ recipientOf' = [recipientOf EXCEPT ![ns] = toShard]
    /\ chunklessOf' = [chunklessOf EXCEPT ![ns] =
            { s \in Shards : s # toShard /\ css[s][ns].ownsChunks }]
    /\ UNCHANGED << configUUID, nextUUID, unshardsRemaining, css, dropPhase, dropMismatchOn >>

\* The reshard recipient finishes cloning; coordinator advances to commit phase. This is the moment
\* at which the config server is about to swap the UUID in `config.collections`.
EnterCommit(ns) ==
    /\ phase[ns] = PHASE_CLONE
    /\ phase' = [phase EXCEPT ![ns] = PHASE_COMMIT]
    /\ UNCHANGED << configUUID, nextUUID, unshardsRemaining, recipientOf, chunklessOf,
                    css, dropPhase, dropMismatchOn >>

\* Coordinator bumps the config server UUID and refreshes the recipient's CSS entry to match. The
\* recipient ends up owning the single post-unshard chunk; chunkless shards are NOT touched here,
\* which is the bug.
CommitOnConfigAndRecipient(ns) ==
    /\ phase[ns] = PHASE_COMMIT
    /\ LET newUUID == nextUUID
           recipient == recipientOf[ns] IN
        /\ configUUID' = [configUUID EXCEPT ![ns] = newUUID]
        /\ nextUUID' = nextUUID + 1
        /\ css' = [s \in Shards |-> [n \in NameSpaces |->
                IF n = ns /\ s = recipient
                    THEN [uuid |-> newUUID, ownsChunks |-> TRUE]
                ELSE IF n = ns /\ s \in chunklessOf[ns]
                    \* >>> BUG SITE: chunkless shard's CSS keeps the pre-unshard UUID. <<<
                    \* `ownsChunks` is correctly flipped to FALSE (the shard lost its chunks),
                    \* but `uuid` is not refreshed.
                    THEN [uuid |-> css[s][n].uuid, ownsChunks |-> FALSE]
                ELSE css[s][n] ]]
    /\ UNCHANGED << unshardsRemaining, phase, recipientOf, chunklessOf,
                    dropPhase, dropMismatchOn >>

\* SPM-3961 fix: as part of reshard commit, refresh every chunkless shard's CSS UUID so it matches
\* the new config server UUID. Modelled as a separate action because it represents the (future)
\* shard-catalog-authoritative commit step that does not exist on master today.
RefreshChunklessShardCSS(ns) ==
    /\ FIX_ENABLED
    /\ phase[ns] = PHASE_COMMIT
    /\ css[recipientOf[ns]][ns].uuid = configUUID[ns]  \* recipient already refreshed.
    /\ css' = [s \in Shards |-> [n \in NameSpaces |->
            IF n = ns /\ s \in chunklessOf[ns]
                THEN [uuid |-> configUUID[ns], ownsChunks |-> FALSE]
            ELSE css[s][n] ]]
    /\ UNCHANGED << configUUID, nextUUID, unshardsRemaining, phase, recipientOf, chunklessOf,
                    dropPhase, dropMismatchOn >>

\* Coordinator marks the unshard as done.
FinishUnshard(ns) ==
    /\ phase[ns] = PHASE_COMMIT
    /\ configUUID[ns] = css[recipientOf[ns]][ns].uuid   \* recipient must have been refreshed.
    /\ phase' = [phase EXCEPT ![ns] = PHASE_DONE]
    /\ unshardsRemaining' = unshardsRemaining - 1
    /\ UNCHANGED << configUUID, nextUUID, recipientOf, chunklessOf, css,
                    dropPhase, dropMismatchOn >>

\* Downstream dropCollection (the shard-catalog-authoritative variant from SPM-3961) starts a scan.
StartDrop(ns) ==
    /\ phase[ns] = PHASE_DONE
    /\ dropPhase[ns] = DROP_IDLE
    /\ dropPhase' = [dropPhase EXCEPT ![ns] = DROP_SCANNING]
    /\ UNCHANGED << configUUID, nextUUID, unshardsRemaining, phase, recipientOf, chunklessOf,
                    css, dropMismatchOn >>

\* dropCollection scan visits a shard and tassert()s on UUID mismatch. The scan only checks shards
\* whose CSS is populated for this ns (an empty CSS is benign).
ScanShardForDrop(ns, s) ==
    /\ dropPhase[ns] = DROP_SCANNING
    /\ CSSPopulated(s, ns)
    /\ IF css[s][ns].uuid = configUUID[ns]
        THEN /\ UNCHANGED << dropPhase, dropMismatchOn >>
        ELSE /\ dropPhase' = [dropPhase EXCEPT ![ns] = DROP_FAILED]
             /\ dropMismatchOn' = [dropMismatchOn EXCEPT ![ns] = s]
    /\ UNCHANGED << configUUID, nextUUID, unshardsRemaining, phase, recipientOf, chunklessOf, css >>

\* dropCollection scan completes without finding a mismatch.
FinishDropOK(ns) ==
    /\ dropPhase[ns] = DROP_SCANNING
    /\ \A s \in Shards : CSSPopulated(s, ns) => css[s][ns].uuid = configUUID[ns]
    /\ dropPhase' = [dropPhase EXCEPT ![ns] = DROP_OK]
    /\ UNCHANGED << configUUID, nextUUID, unshardsRemaining, phase, recipientOf, chunklessOf,
                    css, dropMismatchOn >>

(**************************************************************************************************)
(* Next-state relation.                                                                           *)
(**************************************************************************************************)

Next ==
    \/ \E ns \in NameSpaces, to \in Shards : StartUnshard(ns, to)
    \/ \E ns \in NameSpaces : EnterCommit(ns)
    \/ \E ns \in NameSpaces : CommitOnConfigAndRecipient(ns)
    \/ \E ns \in NameSpaces : RefreshChunklessShardCSS(ns)
    \/ \E ns \in NameSpaces : FinishUnshard(ns)
    \/ \E ns \in NameSpaces : StartDrop(ns)
    \/ \E ns \in NameSpaces, s \in Shards : ScanShardForDrop(ns, s)
    \/ \E ns \in NameSpaces : FinishDropOK(ns)
    \* Termination stuttering.
    \/ ( unshardsRemaining = 0
         /\ \A ns \in NameSpaces : phase[ns] \in {PHASE_IDLE, PHASE_DONE}
         /\ \A ns \in NameSpaces : dropPhase[ns] \in {DROP_IDLE, DROP_OK, DROP_FAILED}
         /\ UNCHANGED vars )

Spec == Init /\ [][Next]_vars

(**************************************************************************************************)
(* Type invariant.                                                                                *)
(**************************************************************************************************)

TypeOK ==
    /\ configUUID \in [NameSpaces -> Nat]
    /\ nextUUID \in Nat
    /\ unshardsRemaining \in 0..UNSHARDS
    /\ phase \in [NameSpaces -> Phases]
    /\ recipientOf \in [NameSpaces -> Shards]
    /\ chunklessOf \in [NameSpaces -> SUBSET Shards]
    /\ css \in [Shards -> [NameSpaces -> CSSEntryFormat]]
    /\ dropPhase \in [NameSpaces -> DropPhases]

(**************************************************************************************************)
(* Correctness properties.                                                                        *)
(**************************************************************************************************)

\* The property the ticket cares about: every populated per-shard CSS entry, for a namespace that
\* has finished an unshard, agrees with the config server's UUID. Chunkless shards' CSS UUIDs are
\* in scope; the divergence the bug describes is exactly a violation of this invariant.
CSSUUIDMatchesConfigServerOrIsCleared ==
    \A ns \in NameSpaces :
        phase[ns] = PHASE_DONE =>
            \A s \in Shards :
                CSSPopulated(s, ns) => css[s][ns].uuid = configUUID[ns]

\* The downstream tassert (from the SPM-3961 dropCollection work) must never fire. When the spec
\* runs with FIX_ENABLED = TRUE this holds; with FIX_ENABLED = FALSE TLC produces a counterexample
\* in which dropPhase[ns] = DROP_FAILED and dropMismatchOn[ns] is some chunkless shard.
DropCollectionNeverTassertsOnCSSMismatch ==
    \A ns \in NameSpaces : dropPhase[ns] # DROP_FAILED

\* Bait invariants (enable one at a time in the cfg file to generate a counterexample trace).
\* BaitReachedDrop: produces a trace that drives an unshard to completion and starts the drop scan.
BaitReachedDrop ==
    ~ \E ns \in NameSpaces : dropPhase[ns] \in {DROP_SCANNING, DROP_OK, DROP_FAILED}

\* BaitChunklessExists: produces a trace where some shard is chunkless for some completed unshard.
BaitChunklessExists ==
    ~ \E ns \in NameSpaces : phase[ns] = PHASE_DONE /\ chunklessOf[ns] # {}
====================================================================================================
