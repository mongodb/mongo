# SLS-5021: LCG Majority-Loss Disaster Recovery Orchestration

## Problem

A Log Consensus Group (LCG) tolerates the loss of a minority of its members
without operator intervention. Permanent loss of a majority — 2 of 3, or 3 of
5 — is irrecoverable by the consensus protocol alone: no quorum can be
assembled, writes block, and the on-disk state of the survivors is not
authoritative because they may have lagged the lost majority by some number of
acknowledged LSNs.

Recovery from this state requires orchestration outside the consensus
protocol: pick survivors, replay from object-store snapshots, attest that the
chosen survivors carry the highest reachable LSN, and reform the LCG. Done
naively, the procedure can silently drop already-acknowledged commits.

## Procedure

1. **Detect.** The CRS surfaces no-quorum from LCG heartbeats within 30s of
   the loss event and raises a P0 stamped with affected shard / cell / AZ.
2. **Enumerate survivors.** The operator runs the recovery CLI; it enumerates
   live LCG members and the set of permanently lost nodes.
3. **Query object store.** For each LCG member (live or lost) the CLI fetches
   the highest snapshot LSN previously uploaded by that member. Lost-node
   snapshots are still durable; they are a first-class source of truth.
4. **Compute the reachable ceiling.** `R = max(survivor on-disk LSNs ∪
   survivor snapshots ∪ lost-node snapshots)`. The new LCG must reach `R`
   before accepting writes.
5. **Attest.** The operator records a structured attestation event
   (`lcg.recovery.attest`) carrying `{generation, survivors, R, evidence}`.
   The attestation is the runbook signal — a human-in-the-loop checkpoint —
   without which the recovery action refuses to proceed.
6. **Hydrate.** Survivors whose on-disk state is below `R` replay from the
   object-store snapshot until their log catches up.
7. **Form new LCG.** The CRS issues the reconfig that promotes the survivors
   into a new generation. The fence assertion (see Hooks) rejects any write
   below `R`.

## Risk: split-brain if the old majority recovers

If a previously-lost node's storage is later recovered (e.g. an AZ comes back
online with cold storage intact) it must not be allowed to rejoin and acknowledge writes
into the old generation. Two safeguards:

- The recovery action increments the LCG `generation` counter. Any RPC
  carrying a stale generation is rejected at the message layer.
- The monotonic-LSN-fence assertion (below) refuses to durably persist
  writes whose LSN sits below the recovery ceiling `R`, even if a stale
  member is briefly reachable and tries to push older entries.

The formal counter-example for skipping these safeguards is in
`src/mongo/tla_plus/Disagg/LCGMajorityLossRecovery/`: the bug-mode `.cfg`
disables the attestation gate and TLC produces a trace where a committed LSN
is dropped by the successor LCG.

## Hooks

- **Structured logging.** Every state transition emits a JSON event under
  `lcg.recovery.*`: `detect`, `enumerate`, `attest`, `hydrate`, `reconfig`,
  `complete`. Each event carries the LCG id, generation, survivor set,
  reachable ceiling, and elapsed-time-from-detect. Dashboards plot the
  full timeline; auditors replay it offline.
- **Runbook signal.** The attestation step is gated on an operator-issued CLI
  call that writes a signed marker to the config server. Until the marker is
  present, the recovery action is a no-op — the orchestration cannot bypass
  the human checkpoint by retry alone.
- **Monotonic-LSN-fence assertion.** Each survivor refuses to persist a log
  entry whose LSN is below the recovery ceiling `R` recorded at reconfig
  time. The fence is a `tassert` at the storage-engine boundary so violation
  crashes loudly rather than producing silent divergence. The fence value is
  written into the WT durable history at reconfig commit, so it survives
  restarts and is verifiable by an external auditor.

## Verification

The TLA+ spec at
`src/mongo/tla_plus/Disagg/LCGMajorityLossRecovery/LCGMajorityLossRecovery.tla`
models a 5-node LCG with explicit `AcceptWrite`, `Snapshot`, `MajorityLoss`,
`Attest`, `Recover`, and `Hydrate` actions. The headline safety invariant
`NoCommittedDataLoss` holds under the canonical config
(`MCLCGMajorityLossRecovery.cfg`). Flipping `RequireAttestation = FALSE` in
`MCLCGMajorityLossRecoveryBug.cfg` produces a counter-example where the
successor LCG diverges from the original commit ledger — proof that the
attestation gate is load-bearing, not bureaucratic.
