/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * Parent role: orchestrator and verifier. Spawns one or two symmetric nodes per the -r topology,
 * then drives a one-second-tick timeline: a role switch every -s seconds (directed through the
 * switch-request sentinel and confirmed through the numbered switch-done sentinel), SIGKILLs at the
 * -k times (targeting whichever node currently holds the role), and a graceful stop at the -t
 * timeout. Children dying at any other point fail the test.
 *
 * Afterwards the parent reopens the surviving state and verifies it against the record files. It
 * never opens WiredTiger while children are running.
 */

#include "schema_disagg_abort.h"

#include <signal.h>

#include "subproc.h"

static void die_on_child_status(const SUBPROC *proc, SUBPROC_STATUS status, int code)
  WT_GCC_FUNC_DECL_ATTRIBUTE((noreturn));

/* The spawned children, so that a failing parent can take them down with it. */
static SUBPROC *live_children = NULL;

/*
 * kill_children --
 *     Kill whatever is still running when the test fails. Without this a failure leaves the
 *     surviving node behind: nothing else ever reaps it, and it runs on for as long as the machine
 *     is up, competing with later runs.
 */
static void
kill_children(void)
{
    if (live_children == NULL)
        return;

    for (size_t i = 0; i < SUBPROC_SLOTS; ++i)
        if (live_children[i].who != NULL && !live_children[i].reaped)
            (void)kill(live_children[i].pid, SIGKILL);
}

/*
 * create_test_dirs --
 *     Create the directory structure needed for a fresh test run.
 */
static void
create_test_dirs(const TEST_CONFIG *cfg, uint32_t nnodes)
{
    char buf[PATH_MAX];

    testutil_recreate_dir(cfg->home);
    testutil_snprintf(buf, sizeof(buf), "%s/%s", cfg->home, RECORDS_DIR);
    testutil_mkdir(buf);
    testutil_snprintf(buf, sizeof(buf), "%s/%s", cfg->home, PAGE_LOG_DIR);
    testutil_mkdir(buf);
    for (uint32_t id = 0; id < nnodes; id++) {
        testutil_snprintf(buf, sizeof(buf), "%s/" NODE_HOME_FMT, cfg->home, id);
        testutil_mkdir(buf);
    }
}

/*
 * spawn_node --
 *     Spawn this binary again as a node, passing the full configuration on the command line. The
 *     node inherits only its own two pipe ends; the peer's ends are closed in the child so pipe EOF
 *     still signals the peer's death.
 */
static void
spawn_node(const TEST_CONFIG *cfg, const char *self_path, uint32_t node_id, bool leader,
  int read_fd, int write_fd, const int *close_fds, size_t nclose, SUBPROC *proc)
{
    static const char *const node_names[SUBPROC_SLOTS] = {"node0", "node1"};
    testutil_assert(node_id < SUBPROC_SLOTS);

    char id_arg[16], nth_arg[16], pool_arg[16], rfd_arg[16], switch_arg[16], wfd_arg[16],
      seed_arg[80];
    testutil_snprintf(id_arg, sizeof(id_arg), "%" PRIu32, node_id);
    testutil_snprintf(nth_arg, sizeof(nth_arg), "%" PRIu32, cfg->nth);
    testutil_snprintf(pool_arg, sizeof(pool_arg), "%" PRIu32, cfg->pool_size);
    testutil_snprintf(rfd_arg, sizeof(rfd_arg), "%d", read_fd);
    testutil_snprintf(switch_arg, sizeof(switch_arg), "%" PRIu32, cfg->switch_interval);
    testutil_snprintf(wfd_arg, sizeof(wfd_arg), "%d", write_fd);
    testutil_snprintf(seed_arg, sizeof(seed_arg), TESTUTIL_SEED_FORMAT, cfg->opts->data_seed,
      cfg->opts->extra_seed);

    const char *argv[28];
    size_t n = 0;
    argv[n++] = self_path;
    argv[n++] = "-r";
    argv[n++] = "node";
    argv[n++] = "-i";
    argv[n++] = id_arg;
    argv[n++] = "-A";
    argv[n++] = leader ? "l" : "f";
    argv[n++] = "-h";
    argv[n++] = cfg->opts->home;
    if (cfg->opts->build_dir != NULL) {
        argv[n++] = "-b";
        argv[n++] = cfg->opts->build_dir;
    }
    argv[n++] = "-T";
    argv[n++] = nth_arg;
    argv[n++] = "-u";
    argv[n++] = pool_arg;
    if (cfg->unique_tables)
        argv[n++] = "-q";
    /* The node bounds how far its generator runs ahead so a hand-over drains inside a period. */
    if (cfg->switch_interval != 0) {
        argv[n++] = "-s";
        argv[n++] = switch_arg;
    }
    if (read_fd >= 0) {
        argv[n++] = "-R";
        argv[n++] = rfd_arg;
        argv[n++] = "-W";
        argv[n++] = wfd_arg;
    }
    argv[n++] = seed_arg;
    argv[n] = NULL;

    subproc_spawn(proc, node_names[node_id], self_path, (char *const *)argv, close_fds, nclose);
}

/*
 * die_on_child_status --
 *     Fail the test, reporting how a child terminated.
 */
static void
die_on_child_status(const SUBPROC *proc, SUBPROC_STATUS status, int code)
{
    testutil_die(EINVAL, "%s terminated unexpectedly: %s %d", proc->who,
      status == SUBPROC_EXITED ? "exit code" : "signal", code);
}

/*
 * child_alive --
 *     Report whether a slot holds a spawned, still-running child.
 */
static bool
child_alive(SUBPROC *proc)
{
    if (proc->who == NULL || proc->reaped)
        return (false);

    int code;
    const SUBPROC_STATUS status = subproc_poll(proc, &code);
    if (status != SUBPROC_RUNNING)
        die_on_child_status(proc, status, code); /* Dying on its own fails the test. */
    return (true);
}

/*
 * children_poll --
 *     Health-check every spawned child, failing the test if one died on its own; reports how many
 *     are still running.
 */
static uint32_t
children_poll(SUBPROC children[SUBPROC_SLOTS])
{
    uint32_t alive = 0;
    for (size_t i = 0; i < SUBPROC_SLOTS; ++i)
        if (child_alive(&children[i]))
            ++alive;
    return (alive);
}

/*
 * wait_for_sentinel --
 *     Wait up to the given number of seconds for the children to create a sentinel file, failing if
 *     a child dies or the wait times out first.
 */
static void
wait_for_sentinel(
  const TEST_CONFIG *cfg, SUBPROC children[SUBPROC_SLOTS], const char *sentinel, uint32_t max_wait)
{
    for (uint32_t waited = 0; !testutil_exists(cfg->home, sentinel); ++waited) {
        if (children_poll(children) == 0)
            testutil_die(EINVAL, "no live children while waiting for %s", sentinel);
        if (waited >= max_wait)
            testutil_die(
              ETIMEDOUT, "%s was not created within %" PRIu32 " seconds", sentinel, max_wait);
        sleep(1);
    }
}

/*
 * reap_child --
 *     Wait for a child to terminate and assert it ended the expected way: SIGKILLed on our signal,
 *     or a clean exit after the stop sentinel. A child that outlives the wait is killed and the
 *     test failed.
 */
static void
reap_child(SUBPROC *proc, SUBPROC_STATUS want_status, int want_code)
{
    int code;
    SUBPROC_STATUS status;

    for (uint32_t waited = 0; (status = subproc_poll(proc, &code)) == SUBPROC_RUNNING; ++waited) {
        if (waited >= MAX_WAIT) {
            subproc_kill(proc);
            (void)subproc_wait(proc, &code);
            testutil_die(ETIMEDOUT, "%s did not terminate within %d seconds", proc->who, MAX_WAIT);
        }
        sleep(1);
    }

    if (status != want_status || code != want_code)
        die_on_child_status(proc, status, code);
}

/*
 * direct_switch --
 *     Direct one role switch and wait for its completion: the current leader steps down and hands
 *     over to a live peer, or a lone survivor flips its role by itself. Updates the parent's view
 *     of who holds which role. Skipped when no child is left to act.
 */
static void
direct_switch(const TEST_CONFIG *cfg, SUBPROC children[SUBPROC_SLOTS], int *cur_leaderp,
  int *cur_followerp, uint32_t *genp)
{
    const int leader = *cur_leaderp, follower = *cur_followerp;
    const bool leader_alive = leader >= 0 && child_alive(&children[leader]);
    const bool follower_alive = follower >= 0 && child_alive(&children[follower]);

    if (!leader_alive && !follower_alive) {
        println("Parent: skipping switch, no live children");
        return;
    }

    const uint32_t gen = ++*genp;
    println("Parent: directing switch %" PRIu32, gen);
    testutil_sentinel(cfg->home, SWITCH_REQUEST_FILE);

    char done_name[64];
    testutil_snprintf(done_name, sizeof(done_name), SWITCH_DONE_FMT, gen);
    wait_for_sentinel(cfg, children, done_name, 4 * MAX_WAIT);

    /* Live nodes swapped roles; a dead slot leaves its new role vacant. */
    *cur_leaderp = follower_alive ? follower : -1;
    *cur_followerp = leader_alive ? leader : -1;
}

/*
 * run_children --
 *     Run the scenario: spawn the node(s) per the topology, then drive the timeline of switches,
 *     kills, and the final graceful stop.
 */
static void
run_children(TEST_CONFIG *cfg, const char *self_path)
{
    const uint32_t nnodes = (cfg->with_leader ? 1u : 0u) + (cfg->with_follower ? 1u : 0u);
    static SUBPROC children[SUBPROC_SLOTS];
    WT_CLEAR(children);

    /* Any failure from here on kills the children before it aborts the process. */
    live_children = children;
    custom_die = kill_children;

    create_test_dirs(cfg, nnodes);

    if (nnodes == 1)
        spawn_node(cfg, self_path, 0, cfg->with_leader, -1, -1, NULL, 0, &children[0]);
    else {
        /*
         * One pipe per direction, each named for the node that reads it: a node writes to its
         * peer's pipe while leading and reads its own while following.
         */
        int to_node0[2], to_node1[2];
        subproc_pipe(to_node0);
        subproc_pipe(to_node1);

        /* node0 keeps the to_node1 write and the to_node0 read. */
        const int close0[] = {to_node1[0], to_node0[1]};
        spawn_node(cfg, self_path, 0, true, to_node0[0], to_node1[1], close0, WT_ELEMENTS(close0),
          &children[0]);
        /* node1 keeps the to_node0 write and the to_node1 read. */
        const int close1[] = {to_node1[1], to_node0[0]};
        spawn_node(cfg, self_path, 1, false, to_node1[0], to_node0[1], close1, WT_ELEMENTS(close1),
          &children[1]);

        /* The children own the pipes now; each pipe's ends live only in its two users. */
        close(to_node0[0]);
        close(to_node0[1]);
        close(to_node1[0]);
        close(to_node1[1]);
    }

    /*
     * Start the clock only once the workload is productive: a leader's first checkpoint can run
     * long under heavy schema churn, so give it a much wider window than the follower, whose first
     * pickup follows promptly once a checkpoint exists. A lone follower has nothing to wait for.
     */
    if (cfg->with_leader)
        wait_for_sentinel(cfg, children, LEADER_READY_FILE, 4 * MAX_WAIT);
    if (nnodes == 2)
        wait_for_sentinel(cfg, children, FOLLOWER_READY_FILE, MAX_WAIT);

    int cur_leader = cfg->with_leader ? 0 : -1;
    int cur_follower = nnodes == 2 ? 1 : (cfg->with_follower ? 0 : -1);
    uint32_t switch_gen = 0;
    uint32_t next_switch = cfg->switch_interval;

    for (uint32_t elapsed = 1; elapsed <= cfg->total_time; ++elapsed) {
        (void)children_poll(children); /* Dying on its own fails the test. */
        sleep(1);

        /* Kills first: overlapping a kill with a same-tick switch is the interesting order. */
        SUBPROC *targets[KILL_TARGETS];
        size_t ntargets = 0;
        for (int k = 0; k < KILL_TARGETS; ++k) {
            if (cfg->kill_time[k] != elapsed)
                continue;
            /* The lone node lives in slot 0; role targets follow the parent's tracking. */
            const int slot = k == KILL_LONE ? 0 : (k == KILL_LEADER ? cur_leader : cur_follower);
            if (slot < 0 || !child_alive(&children[slot]))
                continue;
            targets[ntargets++] = &children[slot];
            if (cur_leader == slot)
                cur_leader = -1;
            if (cur_follower == slot)
                cur_follower = -1;
        }
        /* Kill before reaping so simultaneous deaths overlap as much as possible. */
        for (size_t i = 0; i < ntargets; ++i) {
            println("Parent: killing %s at %" PRIu32 "s", targets[i]->who, elapsed);
            subproc_kill(targets[i]);
        }
        for (size_t i = 0; i < ntargets; ++i)
            reap_child(targets[i], SUBPROC_KILLED, SIGKILL);

        if (cfg->switch_interval != 0 && elapsed == next_switch) {
            next_switch += cfg->switch_interval;
            direct_switch(cfg, children, &cur_leader, &cur_follower, &switch_gen);
        }
    }

    println("Parent: directing graceful stop");
    testutil_sentinel(cfg->home, STOP_FILE);
    for (size_t i = 0; i < SUBPROC_SLOTS; ++i)
        if (children[i].who != NULL && !children[i].reaped)
            reap_child(&children[i], SUBPROC_EXITED, EXIT_SUCCESS);
}

/*
 * open_for_recovery --
 *     Open the given node home as a disaggregated leader to trigger recovery.
 */
static void
open_for_recovery(const TEST_CONFIG *cfg, uint32_t node_id, WT_CONNECTION **connp)
{
    char home_dir[32];
    testutil_snprintf(home_dir, sizeof(home_dir), NODE_HOME_FMT, node_id);

    disagg_opts_init(cfg);
    cfg->opts->disagg.mode = "leader";

    testutil_wiredtiger_open(cfg->opts, home_dir, "create,disaggregated=(lose_all_my_data=true)",
      NULL, connp, true, false);
}

/*
 * verify_homes --
 *     Reopen and verify the surviving state of every node home that exists against the union of the
 *     nodes' records, then check the relay's integrity.
 */
static void
verify_homes(const TEST_CONFIG *cfg)
{
    for (uint32_t id = 0; id < MAX_NODES; id++) {
        char home_dir[32];
        testutil_snprintf(home_dir, sizeof(home_dir), NODE_HOME_FMT, id);
        if (!testutil_exists(NULL, home_dir))
            continue;

        println("Parent: Open node%" PRIu32 " database, run recovery and verify content", id);

        WT_CONNECTION *conn;
        open_for_recovery(cfg, id, &conn);
        verify_schema_state(conn, cfg);
        testutil_check(conn->close(conn, "debug=(skip_checkpoint=true)"));
    }

    verify_relay_prefix(cfg);
}

/*
 * parent_main --
 *     Parent role entry point: run the scenario, then verify the outcome. Any failure aborts the
 *     process, so returning at all means success.
 */
void
parent_main(TEST_CONFIG *cfg, const char *self_path)
{
    char cwd_start[PATH_MAX];
    testutil_assert_errno(getcwd(cwd_start, sizeof(cwd_start)) != NULL);

    if (!cfg->verify_only)
        run_children(cfg, self_path);

    if (chdir(cfg->home) != 0)
        testutil_die(errno, "parent chdir: %s", cfg->home);

    if (!cfg->verify_only)
        testutil_copy_data();

    if (cfg->page_log_home[0] == '\0')
        testutil_snprintf(cfg->page_log_home, sizeof(cfg->page_log_home), "%s/%s/%s", cwd_start,
          cfg->home, PAGE_LOG_DIR);

    verify_homes(cfg);

    if (chdir(cwd_start) != 0)
        testutil_die(errno, "root chdir: %s", cfg->home);

    if (!cfg->opts->preserve)
        testutil_remove(cfg->home);

    testutil_cleanup(cfg->opts);
}
