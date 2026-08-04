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
 * leader's source is its own generator thread writing the full stream (the workload, checkpoint
 * events, and the switch event that ends a term) to a self-pipe; a follower's source is the peer's
 * relay. The remaining role differences: on a checkpoint event a leader produces the checkpoint
 * (and relays the event), a follower picks it up; and a leader relays every applied event to the
 * peer. A follower with no event source (no peer, or the peer died) simply idles in role.
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
#define MAX_CKPT_INVL 4
#define INSERT_ODDS 64          /* generate an insert: 1 in N visits to an insertable slot */
#define EBUSY_CKPT_ATTEMPTS 50  /* worker: retries of a blocked op before it checkpoints itself */
#define GEN_APPLY_RATE_FLOOR 30 /* generator: applied values/second, the multi-node worst case */
#define GEN_LEAD_MIN 64         /* generator: lead floor, so the workers stay fed */
#define SCHEMA_EPOCH_BOOTSTRAP 1
#define MAX_NODES 2
#define MAX_OP_WAIT 30 /* in-node: a retried op gives up, before the parent stops waiting */
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
#define SCHEMA_TABLE_FMT "table:schema_%" PRIu32 "_%" PRIu32 "_%" PRIu32 /* node, thread, slot */

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

/* Connection config. */
#define ENV_CONFIG_DEF "create,statistics=(all),statistics_log=(json,on_close,wait=1)"

/* Which process this instance of the binary is. */
typedef enum { ROLE_PARENT = 0, ROLE_NODE } TEST_ROLE;

/* Timed SIGKILL targets; leader/follower refer to the CURRENT role at kill time. */
typedef enum { KILL_LONE = 0, KILL_LEADER, KILL_FOLLOWER } KILL_TARGET;
#define KILL_TARGETS 3

/*
 * Events, the single currency both roles run on: the leader executes each one and relays it to its
 * peer, which executes it identically. A schema operation is split in two - the operation, then a
 * publish at a fresh epoch - so the independently paced checkpoints land between them, the window
 * this test targets.
 *
 * EVENT_SWITCH ends a term's stream, carrying the term's final counter value: a relay-integrity
 * check the receiver asserts against its own once drained, and the boundary the leaving leader arms
 * its step-down at.
 */
typedef enum {
    EVENT_NONE = 0,
    EVENT_CREATE,
    EVENT_DROP,
    EVENT_INSERT,
    EVENT_PUBLISH_CREATE,
    EVENT_PUBLISH_DROP,
    EVENT_CKPT,
    EVENT_SWITCH
} EVENT_TYPE;

typedef struct {
    EVENT_TYPE type;
    uint32_t thread_id;
    /*
     * The value from the single monotonic allocator whose completion this event represents: a
     * publish epoch, an insert's commit timestamp (also the row value), or a term's final counter.
     * CREATE/DROP carry none - a schema operation completes with its publish.
     */
    uint64_t event_ts;
    uint32_t key_min;
    uint32_t key_max;
    char uri[64];
} SCHEMA_EVENT;

/*
 * The generator's per-slot position in the table lifecycle, valid transitions only. The waiting
 * states hold the slot until a completed checkpoint covers the value in table_wait_ts; the
 * unpublished states linger a random number of visits before the publish (or a cancelling drop),
 * widening the op-publish window the checkpoints land in.
 */
typedef enum {
    TABLE_NONE = 0,         /* slot free: no local table, nothing unpublished */
    TABLE_CREATED,          /* created; the publish may be delayed */
    TABLE_CREATE_PUBLISHED, /* create published, awaiting checkpoint coverage of its epoch */
    TABLE_DIRTY,            /* data committed, awaiting checkpoint coverage of the commit */
    TABLE_DURABLE,          /* create and all data covered: the slot is droppable */
    TABLE_DROPPED,          /* dropped; the publish may be delayed */
    TABLE_DROP_PUBLISHED    /* drop published, awaiting checkpoint coverage of its epoch */
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

/*
 * The phase's shutdown stages, in the order the threads must go: the generator first so the stream
 * ends, then the reader, then the workers draining what it delivered, and last the timestamp
 * thread, whose frontier the checkpoints a draining worker takes for itself are bounded by.
 */
#define STAGE_NONE 0
#define STAGE_GENERATOR 1
#define STAGE_READER 2
#define STAGE_WORKERS 3
#define STAGE_TS 4

/*
 * Workload-engine state, one per node process, owned by node.c.
 */
typedef struct {
    TEST_CONFIG *cfg;    /* bound at creation */
    WT_CONNECTION *conn; /* owned here; open for the node's whole life, since both role
                            transitions are in-place reconfigures */
    /*
     * This phase leads: the generator produces the workload, checkpoint events are produced rather
     * than picked up, and every applied event is relayed to the peer. Fixed per phase.
     */
    bool leads;
    bool generates;         /* this phase generates events into the self-pipe; fixed per phase */
    bool handover_received; /* the term was handed over this phase; atomic access */
    uint32_t stop_stage;    /* how far the phase's shutdown has progressed; atomic access */
    /*
     * The coverage the slot machine's waiting states wait for, republished by the timestamp thread
     * from the connection's last_disaggregated_schema_epoch. Dropping an uncovered table wedges its
     * worker in EBUSY, and through the full queue the checkpoints that would unwedge it. Atomic
     * access.
     */
    uint64_t ckpt_covered_ts;
    uint64_t adopted_ckpt_lsn; /* skip re-adopting the same checkpoint; reset on reopen */
    uint32_t switch_gen;       /* how many role transitions this node has completed */
    /*
     * Single monotonic allocator for schema epochs AND commit timestamps. One counter makes
     * causality an ordering property: a commit into a table always draws a higher value than the
     * table's create, so one stable frontier is consistent on both axes by construction. A follower
     * advances it to every value it applies, and a node stepping up mid-test seeds it from the
     * previous leader's final counter.
     */
    uint64_t current_ts;
    /*
     * In flight is emitted minus applied. Counted rather than derived from the completed frontier:
     * a worker whose picks are all gated never reports one.
     */
    uint64_t emitted; /* generator only */
    uint64_t applied; /* atomic access: every worker adds to it */
    uint32_t nth_workers;
    /* Per-worker-thread state; the reader fills the queue, the slot model is the generator's. */
    struct {
        EVENT_QUEUE evq; /* this worker's inbound events */
        bool busy;       /* the worker is mid-apply; atomic access */
        /*
         * The highest allocator value the thread has fully completed (published or committed). The
         * thread's single in-flight operation always holds a higher value, so the minimum across
         * all workers is a frontier with nothing unfinished at or below it; the timestamp thread
         * sets the stable timestamp and stable schema epoch to that minimum.
         */
        uint64_t completed_ts;
        /* Carried across phases, so each leader phase resumes its namespace where the last ended.
         */
        TABLE_STATE table_state[MAX_POOL_SIZE];
        uint64_t table_wait_ts[MAX_POOL_SIZE]; /* waiting states: the value coverage must reach */
    } workers[MAX_TH];
    /*
     * The generator's random streams, owned by generator.c: one per worker, driving the stream of
     * operations generated for that worker, plus its own pacing stream one past them. Reseeded by
     * workload_start on every phase, generating or not, so they stay in step across role switches.
     */
    WT_RAND_STATE gen_rnd[MAX_TH + 1];
} WORKLOAD_STATE;

/*
 * The reader thread's checkpoint bookkeeping for one phase. Thread-local: it lives on the reader's
 * stack and is handed to the role's checkpoint operation, so the engine holds no per-role state.
 * Each field belongs to one role only.
 */
typedef struct {
    WT_PAGE_LOG *page_log;       /* following: the page log checkpoints are picked up from */
    bool picked_up;              /* following: the first pickup reported readiness */
    struct timespec phase_start; /* leading: when the phase began, bounding the startup skip */
    int produced;                /* leading: checkpoints produced so far */
} CKPT_CTX;

/*
 * A node role, in the C flavor of a vtable: the node's single control loop (the state machine in
 * node.c) runs the phases, and the role differences that are behavior rather than a choice of data
 * dispatch through these operations. leave() steps out of the role once a switch is triggered and
 * enter() completes the transition into it, both carrying the ending term's final counter mark;
 * checkpoint() handles one checkpoint event, producing it or picking it up.
 */
typedef struct {
    const char *name;         /* also the disaggregated connection mode string */
    const char *close_config; /* connection close configuration for a graceful stop */
    bool leads;               /* this role generates events and checkpoints */
    void (*leave)(WORKLOAD_STATE *state, uint64_t final_counter);
    void (*enter)(WORKLOAD_STATE *state, uint64_t final_counter);
    void (*checkpoint)(
      WORKLOAD_STATE *state, WT_SESSION *session, CKPT_CTX *ckpt, const SCHEMA_EVENT *ev);
} NODE_ROLE;

/* main.c */
void println(const char *fmt, ...) WT_GCC_FUNC_DECL_ATTRIBUTE((format(printf, 1, 2)));
uint64_t query_ts(WT_CONNECTION *conn, const char *name);
void set_ts(WT_CONNECTION *conn, const char *name, uint64_t ts);
void set_frontier(WT_CONNECTION *conn, uint64_t ts);

/* leader.c: the leader specifics - checkpoint production and the role transitions. */
extern const NODE_ROLE node_role_leader;

/* follower.c: the follower specifics - checkpoint pickup and the role transitions. */
extern const NODE_ROLE node_role_follower;
void follower_adopt_latest(WORKLOAD_STATE *state);

/* parent.c */
void parent_main(TEST_CONFIG *cfg, const char *self_path);

/*
 * node.c: the generic node - the control loop and role state machine, the connection, the workload
 * engine's state and per-phase lifecycle, the worker event queues, and the timestamp thread.
 */
int node_main(TEST_CONFIG *cfg);
void node_open(TEST_CONFIG *cfg, const char *disagg_mode, WT_CONNECTION **connp);
void disagg_opts_init(const TEST_CONFIG *cfg);
bool node_switch_request_consume(void);
WORKLOAD_STATE *workload_state_create(TEST_CONFIG *cfg);
void workload_start(WORKLOAD_STATE *state, bool as_leader);
void workload_stop(WORKLOAD_STATE *state);
bool workload_active(WORKLOAD_STATE *state, uint32_t stage);
void workload_enqueue(WORKLOAD_STATE *state, const SCHEMA_EVENT *ev);
bool workload_dequeue(WORKLOAD_STATE *state, uint32_t thread_index, SCHEMA_EVENT *ev);
bool workload_queue_empty(WORKLOAD_STATE *state, uint32_t thread_index);
void workload_drain_barrier(WORKLOAD_STATE *state);
void workload_seed_counter(WORKLOAD_STATE *state, uint64_t ts);
void workload_counter_advance(WORKLOAD_STATE *state, uint64_t v);

/* event_pipe.c: the event framing every pipe shares. */
bool node_event_send(TEST_CONFIG *cfg, const SCHEMA_EVENT *ev);
void pipe_event_write(int fd, const SCHEMA_EVENT *ev);
bool pipe_event_read(int fd, SCHEMA_EVENT *ev);
bool pipe_wait_readable(int fd);

/* generator.c: the generator stage - the node's command stream, and all switch triggering. */
void node_generator_start(WORKLOAD_STATE *state);
void node_generator_join(void);

/* reader.c: the reader stage - demuxing the source pipe to the workers. */
void node_reader_start(WORKLOAD_STATE *state);
void node_reader_join(void);

/* worker.c: the worker stage - executing events and recording them. */
void node_workers_start(WORKLOAD_STATE *state);
void node_workers_join(WORKLOAD_STATE *state);

/* verify.c */
void verify_schema_state(WT_CONNECTION *conn, const TEST_CONFIG *cfg);
void verify_relay_prefix(const TEST_CONFIG *cfg);
