/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * Disaggregated schema epoch crash recovery test.
 *
 * The test binary runs in one of two roles, each in its own process:
 *
 *   parent - orchestrator and verifier. Spawns one or two symmetric nodes per the -r topology,
 *            then drives a timeline: role switches every -s seconds, SIGKILLs at the -k times,
 *            and a graceful stop at the -t timeout. Afterwards it reopens the surviving state
 *            and verifies it against the record files.
 *   node   - a database node that leads or follows, described below.
 *
 * Events are the single currency, and both node roles run the identical pipeline: a source writes
 * events to a pipe, the reader demuxes them to per-thread queues, and the workers execute them. A
 * leader's source is its own generator thread writing the full stream (the workload and the switch
 * event that ends a term) to a self-pipe; a follower's source is the peer's relay. The only role
 * difference in the pipeline is that a leader relays every applied event to the peer. A follower
 * with no event source (no peer, or the peer died) simply idles in role.
 *
 * Checkpoints are deliberately not part of the stream: a checkpoint thread paces them on its own,
 * producing them while leading and adopting the latest one while following. That independence is
 * what lets a checkpoint land anywhere between an operation and its publish, and what keeps
 * checkpoints coming while a worker sits blocked waiting for one.
 *
 * Nodes switch roles on the switch event: the leader's generator emits it when the parent drops the
 * switch-request sentinel, and the hand-over relays it to the peer; a lone follower's generator
 * consumes the sentinel and reports the hand-over directly. Nodes stop gracefully when the parent
 * drops the stop sentinel.
 *
 * The children are started by re-spawning this binary with internal options, so no state is
 * inherited by forking: everything a node needs travels through the command line. The two nodes
 * are connected by a pair of pipes, one per direction; a node writes to its out-pipe while
 * leading and reads from its in-pipe while following.
 */

#pragma once

#include "test_util.h"

/* Tunables. */
#define MAX_CKPT_INVL 4 /* checkpoint thread: upper bound on the interval, in seconds */
#define INSERT_ODDS 64  /* generate an insert: 1 in N visits to a published slot */
/*
 * Generate a drop: 1 in N visits to a published slot. It is the dwell in the published state, so it
 * governs how much data a table accumulates before it is dropped - keep it well above INSERT_ODDS
 * or tables are dropped empty and the verifier has nothing to check.
 */
#define DROP_ODDS 48
#define GEN_APPLY_RATE_FLOOR 30 /* generator: applied values/second, the multi-node worst case */
#define GEN_LEAD_MIN 64         /* generator: lead floor, so the workers stay fed */
#define SCHEMA_EPOCH_BOOTSTRAP 1
#define MAX_NODES 2
/*
 * In-node: a retried op gives up, before the parent stops waiting. A blocked drop waits for the
 * checkpoint thread's next checkpoint, so this has to stay well above MAX_CKPT_INVL.
 */
#define MAX_OP_WAIT 30
#define MAX_POOL_SIZE 64
#define MAX_TH 12
#define MAX_TIME 40
#define MAX_WAIT 60 /* parent: a child starting, stopping, or posting a sentinel */
#define MIN_POOL_SIZE 2
#define MIN_TH 2
#define MIN_TIME 10

/* Directory layout under the test home. */
#define NODE_HOME_FMT "WT_NODE%" PRIu32
#define PAGE_LOG_DIR "PALITE"

/* URI / file name patterns; tables and record files are namespaced by owning node. */
#define DATA_KEY_MIN 0
#define DATA_KEY_MAX 9
/*
 * node, thread, slot, generation. The generation stays zero unless -q asks for unique table names,
 * so the name is the slot's whatever the mode, and the verifier parses one format either way.
 */
#define SCHEMA_TABLE_FMT "table:schema_%" PRIu32 "_%" PRIu32 "_%" PRIu32 "_%" PRIu32

/*
 * Per-node, per-thread record files: "<records dir>/node<node>-<role>-<thread>", named for the role
 * the node held when it wrote them. Its "leader" files log the operations it originated; its
 * "follower" files log the peer's events it applied.
 */
#define LEADER_RECORDS_FILE RECORDS_DIR DIR_DELIM_STR "node%" PRIu32 "-leader-%" PRIu32
#define FOLLOWER_RECORDS_FILE RECORDS_DIR DIR_DELIM_STR "node%" PRIu32 "-follower-%" PRIu32

/*
 * Sentinels the nodes and the parent coordinate through. The switch request is consumed by the
 * acting node (the leader, or a lone node), so each request fires exactly one switch.
 */
#define LEADER_READY_FILE "leader_ready"       /* initial leader completed a checkpoint */
#define FOLLOWER_READY_FILE "follower_ready"   /* initial follower completed a pickup */
#define SWITCH_REQUEST_FILE "switch_request"   /* parent directs a role switch */
#define SWITCH_DONE_FMT "switch_done.%" PRIu32 /* the n-th switch completed */
#define STOP_FILE "stop_run"                   /* parent directs a graceful stop */

/* The follower's latest adopted checkpoint LSN; a stepping-down leader polls it. */
#define ADOPTED_LSN_FILE "ckpt_adopted"

/* Connection config. */
#define ENV_CONFIG_DEF "create,statistics=(all),statistics_log=(json,on_close,wait=1)"

/* Which process this instance of the binary is. */
typedef enum { ROLE_PARENT = 0, ROLE_NODE } TEST_ROLE;

/* Timed SIGKILL targets; leader/follower refer to the CURRENT role at kill time. */
typedef enum { KILL_LONE = 0, KILL_LEADER, KILL_FOLLOWER } KILL_TARGET;
#define KILL_TARGETS 3

/* Test-wide events. Leader sends them; follower receives them from the peer. */
typedef enum {
    EVENT_NONE = 0,
    EVENT_CREATE,
    EVENT_DROP,
    EVENT_INSERT,
    EVENT_PUBLISH_CREATE,
    EVENT_PUBLISH_DROP,
    EVENT_STEPDOWN,
    EVENT_SWITCH
} EVENT_TYPE;

typedef struct {
    EVENT_TYPE type;
    uint32_t thread_id;
    /*
     * The timestamp whose completion this event represents: a publish epoch, an insert's commit
     * timestamp (which the rows also hold), or a term's final timestamp. CREATE/DROP carry none - a
     * schema operation completes with its publish.
     */
    uint64_t event_ts;
    uint32_t key_min;
    uint32_t key_max;
    char uri[64];
} SCHEMA_EVENT;

/*
 * The generator's per-slot position in the table lifecycle, valid transitions only. Every state
 * lingers a random number of visits before its next move, widening the windows a checkpoint can
 * land in: between a schema operation and its publish, and between a publish and the drop that
 * follows.
 */
typedef enum {
    TABLE_NONE = 0,  /* slot free: no local table, nothing unpublished */
    TABLE_CREATED,   /* created; the publish may be delayed */
    TABLE_PUBLISHED, /* create published; may take data, and is droppable */
    TABLE_DROPPED    /* dropped; the publish may be delayed */
} TABLE_STATE;

/* Test-wide configuration, built from the command line by every role independently. */
typedef struct {
    TEST_OPTS *opts;
    TEST_ROLE role;
    bool with_leader;   /* parent: the -r topology */
    bool with_follower; /* parent: the -r topology */
    uint32_t node_id;   /* this node's id (namespace, homes, records); parent: unused */
    bool start_leader;  /* this node's parent-assigned starting role */
    bool peer_alive;    /* this node has a live peer; cleared on pipe EOF/EPIPE */
    char home[PATH_MAX];
    char page_log_home[PATH_MAX];
    uint32_t nth;
    uint32_t pool_size;
    /* FIXME-WT-18403: Remove -q once all the known create/drop/create issues are gone. */
    bool unique_tables;               /* -q: never reuse a table name */
    uint32_t total_time;              /* -t: graceful stop after this many seconds */
    uint32_t switch_interval;         /* -s: switch roles every N seconds; 0: never */
    uint32_t kill_time[KILL_TARGETS]; /* -k [l|f]N: SIGKILL the target at N; 0: never */
    bool verify_only;
    int pipe_read_fd;       /* this node's in-pipe (peer's events); -1 when lone */
    int pipe_write_fd;      /* this node's out-pipe (events to the peer); -1 when lone */
    int self_pipe_read_fd;  /* this node's own event source; the generator writes it when */
    int self_pipe_write_fd; /* leading, the reader drains it; process lifetime, never closed */
} TEST_CONFIG;

/*
 * Per-worker single-producer/single-consumer event ring: the reader thread produces, the worker
 * consumes. A full ring blocks the reader, which stops draining the source pipe and so
 * backpressures the producer - the peer leader, or the node's own generator.
 */
#define EVQ_SIZE 256
typedef struct {
    SCHEMA_EVENT ev[EVQ_SIZE];
    uint64_t head; /* consumer position; atomic access */
    uint64_t tail; /* producer position; atomic access */
} EVENT_QUEUE;

/*-
 * The phase's shutdown stages:
 *    1. the generator first so the stream ends,
 *    2. then the reader,
 *    3. then the workers draining what it delivered,
 *    4. and last the checkpoint and timestamp threads.
 *
 * Checkpoint and timestamp threads go after the workers because a draining worker may be blocked on
 * a DROP waiting for durable state.
 */
#define STAGE_NONE 0
#define STAGE_GENERATOR 1
#define STAGE_READER 2
#define STAGE_WORKERS 3
#define STAGE_CKPT 4
#define STAGE_TS 5
/*
 * Stage index is also the count of auxiliary threads. One slot per stage; STAGE_NONE and
 * STAGE_WORKERS are unused.
 */
#define AUX_THR_COUNT (STAGE_TS + 1)

/*
 * Workload-engine state, one per node process, owned by node.c.
 */
typedef struct {
    TEST_CONFIG *cfg;          /* bound at creation */
    WT_CONNECTION *conn;       /* owned here; open for the node's life */
    WT_PAGE_LOG *page_log;     /* owned here; held for the node's life */
    bool leads;                /* this phase leads */
    bool generates;            /* this phase generates events into the self-pipe; fixed per phase */
    bool handover_received;    /* the term was handed over this phase; atomic access */
    uint32_t stop_stage;       /* how far the phase's shutdown has progressed; atomic access */
    uint64_t adopted_ckpt_lsn; /* skip re-adopting the same checkpoint; reset on role change */
    uint32_t switch_gen;       /* how many role transitions this node has completed */

    /* Step-down state, zero outside a transition; atomic access. */
    uint64_t stepdown_ts;       /* while set, the timestamp and checkpoint threads hold */
    uint64_t stepdown_ckpt_lsn; /* the step-down checkpoint, once taken */
    bool ts_busy;               /* the timestamp thread is mid-advance; atomic access */

    /* Single monotonic timestamp for schema epochs AND commit timestamps. */
    uint64_t current_ts;
    uint64_t emitted; /* generator: how many events have been emitted */
    uint64_t applied; /* worker: how many events have been applied; atomic access */
    uint32_t nth_workers;

    /* Per-worker-thread state; the reader fills the queue, the slot model is the generator's. */
    struct {
        wt_thread_t thr;       /* this worker's handle */
        EVENT_QUEUE evq;       /* this worker's inbound events */
        bool busy;             /* the worker is mid-apply; atomic access */
        uint64_t completed_ts; /* the latest published or committed timestamp; atomic access */
        /* Table state is carried across leader-follower transitions. */
        TABLE_STATE table_state[MAX_POOL_SIZE];
        /* Advanced by every create under -q, so a slot's table name is never reused. */
        uint32_t slot_gen[MAX_POOL_SIZE];
        /* Slot took an insert while stepping down; not droppable until the step-down completes. */
        bool stepdown_insert[MAX_POOL_SIZE];
    } workers[MAX_TH];

    /* The single-threaded stages, indexed by stage: generator, reader, checkpoint, timestamp. */
    wt_thread_t aux_thr[AUX_THR_COUNT];

    /* Random streams: one per worker, plus one for the checkpoint thread's cadence. */
    WT_RAND_STATE gen_rnd[MAX_TH + 1];
} WORKLOAD_STATE;

/*
 * The checkpoint thread's bookkeeping for one phase.
 */
typedef struct {
    uint32_t produced;           /* checkpoints produced so far */
    struct timespec phase_start; /* when the phase began, used for checkpoint timeouts */
} CKPT_CTX;

/*
 * A node role: leader or follower. The role's checkpoint operation is the only behavior that
 * differs between them, everything else is shared.
 */
typedef struct {
    const char *name;         /* also the disaggregated connection mode string */
    const char *close_config; /* connection close configuration for a graceful stop */
    bool leads;               /* this role generates events and produces checkpoints */
    void (*checkpoint)(WORKLOAD_STATE *state, WT_SESSION *session, CKPT_CTX *ckpt);
} NODE_ROLE;

/* main.c */
void println(const char *fmt, ...) WT_GCC_FUNC_DECL_ATTRIBUTE((format(printf, 1, 2)));
uint64_t query_ts(WT_CONNECTION *conn, const char *name);
void set_stepdown_ts(WT_CONNECTION *conn, uint64_t ts);
void set_frontier(WT_CONNECTION *conn, uint64_t ts);
void adopted_lsn_publish(uint32_t node_id, uint64_t lsn);
uint64_t adopted_lsn_read(void);

/* parent.c */
void parent_main(TEST_CONFIG *cfg, const char *self_path);

/*
 * node.c: the generic node - the control loop and role state machine, the connection, the workload
 * engine's state and per-phase lifecycle, the worker event queues, and the timestamp thread.
 */
int node_main(TEST_CONFIG *cfg);
const NODE_ROLE *node_role(bool leads);
void disagg_opts_init(const TEST_CONFIG *cfg);
bool node_switch_request_consume(void);
bool workload_active(WORKLOAD_STATE *state, uint32_t stage);
bool node_stage_stopped(WORKLOAD_STATE *state, uint32_t stage);
void workload_counter_advance(WORKLOAD_STATE *state, uint64_t v);

/* evq.c: the per-worker event queues - the reader produces, its worker consumes. */
void evq_enqueue(WORKLOAD_STATE *state, const SCHEMA_EVENT *ev);
bool evq_dequeue(WORKLOAD_STATE *state, uint32_t thread_index, SCHEMA_EVENT *ev);
bool evq_is_empty(WORKLOAD_STATE *state, uint32_t thread_index);
void evq_drain_barrier(WORKLOAD_STATE *state);

/* event_pipe.c: the event framing every pipe shares. */
bool pipe_relay_event(TEST_CONFIG *cfg, const SCHEMA_EVENT *ev);
void pipe_event_write(int fd, const SCHEMA_EVENT *ev);
bool pipe_event_read(int fd, SCHEMA_EVENT *ev);
bool pipe_wait_readable(int fd);

/* generator.c: the generator stage - the node's command stream, and all switch triggering. */
WT_THREAD_RET thread_generator_run(void *arg);

/* reader.c: the reader stage - demuxing the source pipe to the workers. */
WT_THREAD_RET thread_reader_run(void *arg);

/* ckpt.c: the checkpoint and timestamps - running independently of the workload - and the roles. */
WT_THREAD_RET thread_ckpt_run(void *arg);
WT_THREAD_RET thread_ts_run(void *arg);
void ckpt_adopt_latest(WORKLOAD_STATE *state);
bool ckpt_latest(WORKLOAD_STATE *state, WT_PAGE_LOG_GET_COMPLETE_CHECKPOINT_ARGS *args);
void leader_checkpoint(WORKLOAD_STATE *state, WT_SESSION *session, CKPT_CTX *ckpt);
void follower_checkpoint(WORKLOAD_STATE *state, WT_SESSION *session, CKPT_CTX *ckpt);

/* worker.c: the worker stage - executing events and recording them. */
void node_workers_start(WORKLOAD_STATE *state);
void node_workers_stop(WORKLOAD_STATE *state);

/* verify.c */
void verify_schema_state(WT_CONNECTION *conn, const TEST_CONFIG *cfg);
void verify_relay_prefix(const TEST_CONFIG *cfg);
