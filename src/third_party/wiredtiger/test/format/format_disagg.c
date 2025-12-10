/*-
 * Public Domain 2014-present MongoDB, Inc.
 * Public Domain 2008-2014 WiredTiger, Inc.
 *
 * This is free and unencumbered software released into the public domain.
 *
 * Anyone is free to copy, modify, publish, use, compile, sell, or
 * distribute this software, either in source code form or as a compiled
 * binary, for any purpose, commercial or non-commercial, and by any
 * means.
 *
 * In jurisdictions that recognize copyright laws, the author or authors
 * of this software dedicate any and all copyright interest in the
 * software to the public domain. We make this dedication for the benefit
 * of the public at large and to the detriment of our heirs and
 * successors. We intend this dedication to be an overt act of
 * relinquishment in perpetuity of all present and future rights to this
 * software under copyright law.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include "format.h"

/*
 * disagg_redirect_output --
 *     Redirect output to a file in the run directory, unless running quietly.
 */
static void
disagg_redirect_output(const char *output_file)
{
    char path[256];

    testutil_snprintf(path, sizeof(path), "%s/%s", g.home, output_file);

    printf("===> Output will be written to %s\n", path);
    printf("     (If you want to watch live, run: tail -f %s)\n\n", path);
    fflush(stdout);

    if (freopen(path, "w", stdout) == NULL)
        testutil_die(errno, "freopen stdout %s", path);
    if (dup2(fileno(stdout), fileno(stderr)) == -1)
        testutil_die(errno, "dup2 stderr->stdout");

    __wt_stream_set_no_buffer(stdout);
    __wt_stream_set_no_buffer(stderr);
}

/*
 * disagg_teardown_multi_node --
 *     Wait for and clean up any follower processes if we're in multi-node disagg mode.
 */
void
disagg_teardown_multi_node(void)
{
    if (!disagg_is_multi_node())
        return;

    if (g.follower_pid > 0) { /* Parent: leader */
        /* Wait for the follower process to exit. */
        testutil_timeout_wait(120, g.follower_pid);
        g.follower_pid = 0;
    }
}

/*
 * disagg_setup_multi_node --
 *     Set up the environment for multi-node disagg, forking follower processes as needed.
 */
void
disagg_setup_multi_node(void)
{
    pid_t pid;
    char follower_home[256];

    if (!disagg_is_multi_node())
        return;

    testutil_snprintf(follower_home, sizeof(follower_home), "%s/follower", g.home);
    memset(&g.checkpoint_metadata, 0, sizeof(g.checkpoint_metadata));

    /*
     * Create required dir before forking to avoid parent/child races. Skip on reopen, since the run
     * directories already exist.
     */
    if (!g.reopen) {
        testutil_recreate_dir(g.home);
        testutil_mkdir(follower_home);
    }

    /* Initialize a shared page log directory path for all nodes. */
    testutil_snprintf(g.home_page_log, sizeof(g.home_page_log), "%s", g.home);

    fflush(NULL);
    pid = fork();
    testutil_assert_errno(pid >= 0);
    if (pid == 0) { /* Child: follower */
        progname = "t[follower]";
        config_single(NULL, "disagg.mode=follower", true);
        path_setup(follower_home);
        disagg_redirect_output("follower.out");
    } else { /* Parent: leader */
        progname = "t[leader]";
        config_single(NULL, "disagg.mode=leader", true);
        disagg_redirect_output("leader.out");
    }

    g.follower_pid = pid;
}

/*
 * disagg_is_multi_node --
 *     Return true if disagg is configured for multi-node.
 */
bool
disagg_is_multi_node(void)
{
    const char *page_log;
    bool disagg_enabled;

    page_log = GVS(DISAGG_PAGE_LOG);
    disagg_enabled = (strcmp(page_log, "off") != 0 && strcmp(page_log, "none") != 0);

    return (disagg_enabled && GV(DISAGG_MULTI));
}

/*
 * disagg_is_mode_switch --
 *     Check if disagg is configured to use "switch" mode.
 */
bool
disagg_is_mode_switch(void)
{
    return (g.disagg_storage_config && strcmp(GVS(DISAGG_MODE), "switch") == 0);
}

/*
 * disagg_switch_roles --
 *     Toggle the current disagg role between "leader" and "follower",
 */
void
disagg_switch_roles(void)
{
    char disagg_cfg[64];

    /*
     * FIXME-WT-15763: WT does not yet support graceful step-downs. Simply reconfiguring WT to step
     * down may cause issues, so we reopen the connection when switching to follower mode.
     */
    if (g.disagg_leader)
        wts_reopen();

    /* Perform step-up or step-down. */
    g.disagg_leader = !g.disagg_leader;
    testutil_snprintf(disagg_cfg, sizeof(disagg_cfg), "disaggregated=(role=\"%s\")",
      g.disagg_leader ? "leader" : "follower");
    testutil_check(g.wts_conn->reconfigure(g.wts_conn, disagg_cfg));

    if (!g.disagg_leader)
        follower_read_latest_checkpoint();

    /* After every switch, verify the contents of each table */
    wts_verify_mirrors(g.wts_conn, NULL, NULL);
}
