# schema_disagg_abort

Stress test for disaggregated (layered) schema operations under crashes and role switches.
One or two nodes share a page log; tables are created, populated, and dropped continuously
while checkpoints move data into shared storage; the parent kills nodes and swaps
leader/follower roles mid-run; a final recovery pass verifies what survived against
per-operation record files.

## Processes

The binary runs in one of two roles, dispatched in `main.c`:

- **parent** — orchestrator and verifier (`parent.c`). Spawns the nodes per the `-r`
  topology, then drives a one-second-tick timeline of switches, kills, and a graceful stop
  (see [Running](#running)); a child dying at any other point fails the test. It opens
  WiredTiger only afterwards, to recover and verify.
- **node** — a database node running as leader or follower (`node.c`; role specifics in
  `leader.c`/`follower.c` behind the `NODE_ROLE` vtable). Spawned by re-executing the binary
  with internal options (`-r node -i <id> -A l|f`, pipe fds via `-R`/`-W`), so nothing is
  inherited by forking. `subproc.[ch]` is the posix_spawn layer.

## Directory layout

One run under the test home (`-h WT_TEST.foo`):

```
WT_TEST.foo/
├── records/                      the verifier's ground truth
│   ├── node<N>-leader-<T>        ops node N ORIGINATED itself, per worker thread T
│   └── node<N>-follower-<T>      peer ops node N APPLIED as follower, per thread T
├── PALITE/                       shared page log — the "disaggregated storage"
├── WT_NODE0/                     node 0's local WiredTiger home
├── WT_NODE1/                     node 1's local WT home (only with -r lf)
├── leader_ready                  ┐
├── follower_ready                │ sentinel files: the parent⇄node
├── switch_request                │ coordination protocol
├── switch_done.<k>               │
└── stop_run                      ┘
WT_TEST.foo.SAVE/                 copy of the home, taken just before verification
```

- **`PALITE/`** — shared by both nodes, and the only durable state that matters. Leader
  checkpoints write pages and checkpoint metadata into it; followers adopt from it via
  `pl_get_complete_checkpoint` → `reconfigure(checkpoint_meta)`. Single-writer is enforced by
  protocol ordering: step-down closes the connection before the peer steps up.
- **`WT_NODE<i>/`** — a node's local database, kept across its role switches; a follower
  reopen does not wipe it. Verification does: it opens with
  `disaggregated=(lose_all_my_data=true)`, deleting the local files and rebuilding the home
  from the page log — hence the `.SAVE` copy, taken first and replaced on every rerun.
- **`records/`** — named for the origin of what they log: `leader` files for the operations the
  node produced itself, `follower` files for the peer's events it applied. Line formats:
  `CREATE <epoch> <uri>`, `DROP <epoch> <uri>`, `INSERT <commit_ts> <kmin> <kmax> <uri>`
  (`record_event_line` defines them). Opened lazily, appended across terms, line-buffered so
  completed lines survive SIGKILL.
- **Sentinels** (home root): `leader_ready` = first checkpoint (the parent starts the clock);
  `follower_ready` = first pickup; `switch_request` = switch command, **consumed (unlinked)**
  by the acting node so each request fires once; `switch_done.<k>` = k-th switch finished;
  `stop_run` = graceful stop, not consumed.

## Event pipeline

One pipeline, identical in both roles: a *source* writes `SCHEMA_EVENT`s to a pipe, the
reader demuxes them by `thread_id` into per-worker queues, and the workers execute them. What
differs is where the source is.

- **A phase that generates** — a leader always, and a follower with no peer to receive from —
  runs the node's own **generator** thread, writing the term's full stream (workload ops,
  `EVENT_CKPT` at random 0–3 s intervals, `EVENT_SWITCH` to end the term) into a process-local
  **self-pipe**. A leader's workers also relay every applied event to the peer.
- **A phase that consumes** — a follower with a live peer — has no generator at all: its
  source is the peer's relay (one pipe per direction, each named for the node that reads it).
  Nothing is relayed onward.

A peerless follower therefore creates and publishes tables of its own, and never checkpoints
until it steps up: the way a fresh node bootstraps its own tables before taking leadership.

Each workload event carries an `event_ts` from the node-wide monotonic allocator
(`current_ts`): CREATE/DROP the epoch they publish at, INSERT its commit timestamp (also the
row value); `EVENT_SWITCH` carries the term's final counter, `EVENT_CKPT` nothing. One
counter for both axes makes causality an ordering property — a commit always draws a higher
value than its table's create — so one frontier serves both the stable timestamp and the
stable schema epoch. Backpressure is end-to-end: a full ring blocks the reader, a full pipe
blocks the producer.

## Threads (per phase)

Every workload thread is per-phase: `workload_start` creates them, `workload_stop` joins them
in dependency order — the generator first if the phase had one, then the reader, then
`stop_phase` quiesces the workers, which drain their queues. The main thread runs the
`node_run` loop: `workload_start` → `node_trigger_wait` (1 s poll on
`handover_received`/`stop_run`) → `workload_stop` → transition or exit.

| Thread | Count | Does |
|---|---|---|
| generator | 1 iff the phase generates | - `generator_round` feeds every worker one op — pick a slot with that worker's rnd, flip the slot model, allocate `event_ts`, emit CREATE/DROP, and give one create in `INSERT_ODDS` an INSERT at a fresh commit ts; drops of uncovered *dirty* slots are skipped and an empty round sleeps 1 ms</br>- `EVENT_CKPT` every random 0–3 s</br>- `switch_request` polled ~1/s → `EVENT_SWITCH` ends the stream and the phase</br>- holds its lead over the workers to one switch period's work (`GEN_APPLY_RATE_FLOOR`), so a hand-over has little to drain |
| reader | 1 | - `select` (1 s) on the source pipe → demux ops into per-worker rings</br>- `CKPT` → **leader**: `leader_checkpoint` (skipped while stable=0, MAX_STARTUP watchdog); **follower**: drain barrier → `follower_pick_up_checkpoint`</br>- `SWITCH` → drain → assert counter == the sender's final counter → hand over</br>- EOF (peer pipe only) → peer dead: carry on as a lone follower |
| worker ×N | `-T`, ≤ 12 | pop own ring → execute the op (bounded EBUSY retry) → `apply_event`: schema ops record then publish, inserts commit then record → relay to the peer (leader only) → mark completed |
| timestamp | 1 | every 100 ms: frontier = min of workers' `completed_ts`; set oldest/stable/stable-schema-epoch to it (never backwards) |

Coordination is lock-free by design: `stop_phase` quiesces every loop, per-worker SPSC rings plus `busy` flags connect
reader to workers, and `completed_ts[]` feeds the frontier.

## Control loop and transitions

Both roles run the same state machine (`node_run`); only `leave()`/`enter()` differ. The
hand-over value is the node's own quiesced counter: after `workload_stop`, `current_ts` holds
everything the term allocated or adopted. `EVENT_SWITCH` is the stream's last event, so a
hand-over cannot complete until everything already in flight is applied — which is why the
generator's lead is bounded rather than left to the pipe's own capacity.

```
        ┌──────────────────────── LEADER ──────────────────────────────┐
        │ generator: ops + CKPT events ▶ self-pipe                     │
        │            switch_request → EVENT_SWITCH(counter), exit      │
        │ reader   : CKPT   → leader_checkpoint ▶ PALITE → relay       │
        │                     (skip while stable=0; 1st: leader_ready) │
        │            SWITCH → drain → assert counter == sender's final │
        │                     → handover                               │
        │ workers  : execute → record(leader) + publish/commit         │
        │            → relay ▶ peer pipe                               │
        └──────────────────────────────────────────────────────────────┘
  stop_run ──▶ quiesce → close(NULL: closing checkpoint) → exit(0)
  handover ──▶ quiesce → LEAVE: stable := counter → final checkpoint
               → CLOSE conn → send CKPT + SWITCH(counter) [peer alive]
               ENTER follower: reopen conn ──▶ FOLLOWER
  EPIPE on relay ──▶ peer_alive = false (lone leader keeps producing)

        ┌──────────────────────── FOLLOWER ────────────────────────────┐
        │ generator: none when peered; the peer's relay is the source  │
        │            when lone: ops + CKPT events ▶ self-pipe          │
        │            switch_request → EVENT_SWITCH(counter), exit      │
        │ reader   : CKPT   → drain → pickup ◀ PALITE                  │
        │                     (1st: follower_ready)                    │
        │            SWITCH → drain → assert → handover                │
        │            EOF    → peer dead: carry on, now generating      │
        │ workers  : execute → record + publish/commit at the stream's │
        │            event_ts (a follower never relays)                │
        └──────────────────────────────────────────────────────────────┘
  stop_run ──▶ quiesce → close(skip_checkpoint) → exit(0)
  handover ──▶ quiesce → LEAVE: nothing
               ENTER leader: [lone: adopt latest checkpoint first]
               → reconfigure(role=leader) → seed + restore frontier
               from the own counter ──▶ LEADER
```

### Role differences

| Aspect | Leader | Follower |
|---|---|---|
| Event source | own generator → self-pipe | peer's relay → in-pipe; its own generator when lone |
| Counter | generator *allocates* `event_ts` values | *adopts* them from events (`counter_advance`) |
| `EVENT_CKPT` | reader produces the checkpoint, then relays the event (no barrier: it races the workload) | reader barriers, then picks the checkpoint up |
| Relay | workers relay applied events; reader relays `EVENT_CKPT` | — |
| `leave()` | heavy: advance stable to the term's counter, final checkpoint, **close the connection**, hand over | empty |
| `enter()` | reconfigure + seed counter + restore frontier (+ adopt-latest when lone) | reopen the connection |
| Graceful close | `close(NULL)` — closing checkpoint | `close(skip_checkpoint)` |
| Readiness / records | `leader_ready`; `node*-leader-*` | `follower_ready`; `node*-follower-*` |
| Peer-death signal | EPIPE while relaying | pipe EOF |

Both roles react to peer death the same way: keep the role, become "lone"
(`peer_alive = false`). Lone-ness changes three things — the next follower phase generates its
own workload instead of consuming a peer's, a lone step-up adopts the page log's latest
checkpoint before reconfiguring, and the transition reports `switch_done.<k>` itself (a lone
step-down has no peer to hand over to).

## Invariants

1. **`apply_event` ordering**: schema ops record before publishing, so the record reaches the
   file before a checkpoint can make the epoch durable (a record without a durable epoch is
   ignored by the verifier; the reverse would be a hole); inserts commit, then record. Both
   relay *before* the completion store, so the stable frontier — and any checkpoint — only
   covers already-relayed events: the peer holds every event at or below a checkpoint's
   frontier by the time it sees that checkpoint's pipe event.
2. **Drop coverage gating**: a *dirty* table — one written since its create — cannot be
   dropped until a completed checkpoint persists its data, and a fixed stream cannot skip, so
   an uncovered drop would wedge its worker and, through the full ring, the reader and the
   checkpoints that would unwedge it. A DROP is therefore emitted for a slot holding data only
   once its insert commit is at or below `ckpt_covered_ts`, the connection's
   `last_disaggregated_schema_epoch` as republished by the timestamp thread; the pick is
   skipped otherwise. Clean tables are never gated, which is why only one create in
   `INSERT_ODDS` takes data: the schema churn then runs at worker speed and a couple of gated
   slots per checkpoint interval keep exercising the waiting path.
3. **Frontier hand-off**: at step-down the term is quiesced and drained, so `leader_leave`
   advances oldest/stable/stable-epoch to the term's counter before its final checkpoint
   (publishes above that epoch would die with the closed connection), and `leader_enter`
   restores the same frontier so an early checkpoint of the new term cannot regress the
   shared epoch.
4. **Hand-over integrity**: `EVENT_SWITCH` is the last event of a term's stream; the receiver
   drains everything before acting and asserts its counter equals the event's final counter —
   every allocated value rides an event that precedes the switch. The old leader closes its
   connection *before* the switch is sent (one writer per page log).
5. **Uniform EBUSY policy**: workers retry the same operation with a MAX_STARTUP bound; the
   stream is fixed at generation time and the slot model flips when the generator emits, so
   the executed state converges to the model.

## Verification

After the run the parent copies the home to `.SAVE`, recovery-opens every node home that
exists, and checks it against the union of the nodes' `leader` records (`verify.c`):

- `last_disaggregated_schema_epoch` is the durability cutoff: records above it never reached
  a checkpoint and carry no expectations.
- Presence: a slot whose last durable operation was CREATE must exist; DROP must be absent.
- Data: for durable creates the recorded key range must be present, each row valued by its
  commit timestamp (a mismatch means another generation's data); inserts above the last
  checkpoint timestamp are skipped.
- `verify_relay_prefix`: what a node recorded as follower must be an exact line-prefix of what
  its peer recorded as leader, per thread; a partial trailing line is accepted (the recorder
  was killed mid-write).

## Running

```
test_schema_disagg_abort [-b build-dir] [-h dir] [-k [l|f]N] [-p] [-r l|f|lf] [-s N]
                         [-T threads] [-t time] [-u pool] [-v]
```

- `-r` topology: `l` lone leader, `f` lone follower (generates its own workload and never
  checkpoints, so nothing is durable until it steps up), `lf` both; default is a random single
  node.
- `-s N` switches roles every N seconds, each landing within one more period (the hand-over
  drains what is in flight); `-k lN`/`-k fN` SIGKILL the *current* holder of the role at N
  seconds (plain `-k N` for a single node); `-t` stops gracefully.
- `-p` preserves the home; `-v` verifies an existing home only (requires `-T`, `-u`).
- Every run prints a `CONFIG:` line (including the random seeds) that reproduces it.
