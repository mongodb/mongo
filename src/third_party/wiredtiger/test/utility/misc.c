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
#include "test_util.h"

#include <math.h>

#ifndef _WIN32
#include <sys/wait.h>
#endif

#if defined(__APPLE__) || defined(__linux__)
#include <dirent.h>
#include <libgen.h>
#endif

void (*custom_die)(void) = NULL;
const char *progname = "program name not set";

/*
 * testutil_die --
 *     Report an error and abort.
 */
void
testutil_die(int e, const char *fmt, ...)
{
    va_list ap;

    /* Flush output to be sure it doesn't mix with fatal errors. */
    (void)fflush(stdout);
    (void)fflush(stderr);

    fprintf(stderr, "%s: FAILED", progname);
    if (fmt != NULL) {
        fprintf(stderr, ": ");
        va_start(ap, fmt);
        vfprintf(stderr, fmt, ap);
        va_end(ap);
    }
    if (e != 0)
        fprintf(stderr, ": %s", wiredtiger_strerror(e));
    fprintf(stderr, "\n");
    (void)fflush(stderr);

    /* Allow test programs to cleanup on fatal error. */
    if (custom_die != NULL)
        (*custom_die)();

    /* Drop core. */
    fprintf(stderr, "%s: process aborting\n", progname);
    __wt_abort(NULL);
}

/*
 * testutil_set_progname --
 *     Set the global program name for error handling.
 */
const char *
testutil_set_progname(char *const *argv)
{
#ifdef _WIN32
    /*
     * On some Windows environments, such as Cygwin, argv[0] can use '/' as a path delimiter instead
     * of '\\', so check both just in case.
     */
    if ((progname = strrchr(argv[0], '/')) != NULL)
        return (++progname);
#endif

    if ((progname = strrchr(argv[0], DIR_DELIM)) != NULL)
        return (++progname);

    progname = argv[0];
    return (progname);
}

/*
 * testutil_work_dir_from_path --
 *     Takes a buffer, its size and the intended work directory. Creates the full intended work
 *     directory in buffer.
 */
void
testutil_work_dir_from_path(char *buffer, size_t len, const char *dir)
{
    /* If no directory is provided, use the default. */
    if (dir == NULL)
        dir = DEFAULT_DIR;

    if (len < strlen(dir) + 1)
        testutil_die(ENOMEM, "Not enough memory in buffer for directory %s", dir);

    strcpy(buffer, dir);
}

/*
 * testutil_deduce_build_dir --
 *     Deduce the build directory.
 */
void
testutil_deduce_build_dir(TEST_OPTS *opts)
{
    struct stat stats;

    char path[512], pwd[512], stat_path[512];
    char *token;
    int index;

    if (getcwd(pwd, sizeof(pwd)) == NULL)
        testutil_die(ENOENT, "No such directory");

    /* This condition is when the full path name is used for argv0. */
    if (opts->argv0[0] == '/')
        testutil_snprintf(path, sizeof(path), "%s", opts->argv0);
    else
        testutil_snprintf(path, sizeof(path), "%s/%s", pwd, opts->argv0);

    token = strrchr(path, '/');
    while (strlen(path) > 0) {
        testutil_assert(token != NULL);
        index = (int)(token - path);
        path[index] = '\0';

        testutil_snprintf(stat_path, sizeof(stat_path), "%s/wt", path);

        if (stat(stat_path, &stats) == 0) {
            opts->build_dir = dstrdup(path);
            return;
        }
        token = strrchr(path, '/');
    }
    return;
}

/*
 * testutil_build_dir --
 *     Get the build directory.
 */
void
testutil_build_dir(TEST_OPTS *opts, char *buf, int size)
{
    /*
     * To keep it simple, in order to get the build directory we require the user to set the build
     * directory from the command line options. We unfortunately can't depend on a known/constant
     * build directory (the user could have multiple out-of-source build directories). There's also
     * not really any OS-agnostic mechanisms we can here use to discover the build directory the
     * calling test binary exists in.
     */
    if (opts->build_dir == NULL)
        testutil_die(ENOENT, "No build directory given");

    strncpy(buf, opts->build_dir, (size_t)size);
}

/*
 * testutil_progress --
 *     Print a progress message to the progress file.
 */
void
testutil_progress(TEST_OPTS *opts, const char *message)
{
    FILE *fp;
    uint64_t now;

    if (opts->progress_fp == NULL)
        testutil_assert_errno((opts->progress_fp = fopen(opts->progress_file_name, "w")) != NULL);

    fp = opts->progress_fp;
    __wt_seconds(NULL, &now);
    testutil_assert(fprintf(fp, "[%" PRIu64 "] %s\n", now, message) >= 0);
    testutil_assert(fflush(fp) == 0);
}

/*
 * testutil_cleanup --
 *     Delete the existing work directory and free the options structure.
 */
void
testutil_cleanup(TEST_OPTS *opts)
{
    if (opts->conn != NULL)
        testutil_check(opts->conn->close(opts->conn, NULL));

    /*
     * Make sure to close the progress file before we attempt to delete it; otherwise we will get an
     * error on Windows.
     */
    if (opts->progress_fp != NULL)
        testutil_assert(fclose(opts->progress_fp) == 0);

    if (!opts->preserve)
        testutil_remove(opts->home);

    free(opts->uri);
    free(opts->progress_file_name);
    free(opts->home);
    free(opts->build_dir);
    free(opts->tiered_storage_source);
}

/*
 * testutil_copy_data --
 *     Copy the data to a backup folder. Usually, the data copy is cleaned up by a call to
 *     testutil_clean_test_artifacts.
 */
void
testutil_copy_data(const char *dir)
{
    WT_FILE_COPY_OPTS opts;
    char save_dir[512];

    memset(&opts, 0, sizeof(opts));
    opts.preserve = true;

    testutil_snprintf(save_dir, sizeof(save_dir), ".." DIR_DELIM_STR "%s.SAVE", dir);
    testutil_remove(save_dir);
    testutil_copy_ext(".", save_dir, &opts);
}

/*
 * testutil_copy_data_opt --
 *     Copy the data to a backup folder. Directories and files with the specified "readonly prefix"
 *     will be hard-linked instead of copied for efficiency on supported platforms.
 */
void
testutil_copy_data_opt(const char *dir, const char *readonly_prefix)
{
    WT_FILE_COPY_OPTS opts;
    char save_dir[512];

    memset(&opts, 0, sizeof(opts));
    opts.link = true;
    opts.link_if_prefix = readonly_prefix;
    opts.preserve = true;

    testutil_snprintf(save_dir, sizeof(save_dir), ".." DIR_DELIM_STR "%s.SAVE", dir);
    testutil_remove(save_dir);
    testutil_copy_ext(".", save_dir, &opts);
}

/*
 * testutil_clean_test_artifacts --
 *     Clean any temporary files and folders created during test execution
 */
void
testutil_clean_test_artifacts(const char *dir)
{
    char buf[512];

    testutil_snprintf(buf, sizeof(buf), ".." DIR_DELIM_STR "%s.SAVE", dir);
    testutil_remove(buf);

    testutil_snprintf(buf, sizeof(buf), ".." DIR_DELIM_STR "%s.CHECK", dir);
    testutil_remove(buf);

    testutil_snprintf(buf, sizeof(buf), ".." DIR_DELIM_STR "%s.DEBUG", dir);
    testutil_remove(buf);

    testutil_snprintf(buf, sizeof(buf), ".." DIR_DELIM_STR "%s.BACKUP", dir);
    testutil_remove(buf);
}

/*
 * testutil_copy_if_exists --
 *     Copy a file into a directory if it exists.
 */
void
testutil_copy_if_exists(WT_SESSION *session, const char *name)
{
    bool exist;

    testutil_check(__wt_fs_exist((WT_SESSION_IMPL *)session, name, &exist));
    if (exist)
        testutil_copy_file(session, name);
}

/*
 * testutil_verify_model --
 *     Run the model verification tool on the database. The database must be closed, and it has to
 *     be created with debug logging and with log file removal set to false.
 */
void
testutil_verify_model(TEST_OPTS *opts, const char *home)
{
    char tool_path[PATH_MAX];

    testutil_build_dir(opts, tool_path, sizeof(tool_path));
    testutil_strcat(tool_path, sizeof(tool_path), "/test/model/tools/model_verify_debug_log");

    testutil_system("%s -h \"%s\"", tool_path, home);
}

/*
 * testutil_is_flag_set --
 *     Return if an environment variable flag is set.
 */
bool
testutil_is_flag_set(const char *flag)
{
    const char *res;
    bool flag_being_set;

    if (__wt_getenv(NULL, flag, &res) != 0 || res == NULL)
        return (false);

    /*
     * This is a boolean test. So if the environment variable is set to any value other than 0, we
     * return success.
     */
    flag_being_set = res[0] != '0';

    __wt_free(NULL, res);

    return (flag_being_set);
}

/*
 * testutil_print_command_line --
 *     Print command line arguments for csuite tests.
 */
void
testutil_print_command_line(int argc, char *const *argv)
{
    int i;

    printf("Running test command: ");
    for (i = 0; i < argc; i++)
        printf("%s ", argv[i]);
    printf("\n");
}

/*
 * testutil_is_dir_store --
 *     Check if the external storage is dir_store.
 */
bool
testutil_is_dir_store(TEST_OPTS *opts)
{
    bool dir_store;

    dir_store = strcmp(opts->tiered_storage_source, DIR_STORE) == 0 ? true : false;
    return (dir_store);
}

/*
 * testutil_wiredtiger_open --
 *     Call wiredtiger_open with the tiered storage configuration if enabled.
 */
void
testutil_wiredtiger_open(TEST_OPTS *opts, const char *home, const char *config,
  WT_EVENT_HANDLER *event_handler, WT_CONNECTION **connectionp, bool rerun, bool benchmarkrun)
{
    char buf[1024], tiered_cfg[512], tiered_ext_cfg[512];

    opts->local_retention = benchmarkrun ? 0 : 2;
    testutil_tiered_storage_configuration(
      opts, home, tiered_cfg, sizeof(tiered_cfg), tiered_ext_cfg, sizeof(tiered_ext_cfg));

    testutil_snprintf(buf, sizeof(buf), "%s%s%s%s,extensions=[%s]", config == NULL ? "" : config,
      (rerun ? TESTUTIL_ENV_CONFIG_REC : ""), (opts->compat ? TESTUTIL_ENV_CONFIG_COMPAT : ""),
      tiered_cfg, tiered_ext_cfg);

    if (opts->verbose)
        printf("wiredtiger_open configuration: %s\n", buf);
    testutil_check(wiredtiger_open(home, event_handler, buf, connectionp));
}

#ifndef _WIN32
/*
 * testutil_sleep_wait --
 *     Wait for a process up to a number of seconds.
 */
void
testutil_sleep_wait(uint32_t seconds, pid_t pid)
{
    pid_t got;
    int status;

    while (seconds > 0) {
        if ((got = waitpid(pid, &status, WNOHANG | WUNTRACED)) == pid) {
            if (WIFEXITED(status))
                testutil_die(EINVAL, "Child process %" PRIu64 " exited early with status %d",
                  (uint64_t)pid, WEXITSTATUS(status));
            if (WIFSIGNALED(status))
                testutil_die(EINVAL, "Child process %" PRIu64 " terminated  with signal %d",
                  (uint64_t)pid, WTERMSIG(status));
        } else if (got == -1)
            testutil_die(errno, "waitpid");

        --seconds;
        sleep(1);
    }
}
#endif

/*
 * testutil_time_us --
 *     Return the number of microseconds since the epoch.
 */
uint64_t
testutil_time_us(WT_SESSION *session)
{
    struct timespec ts;

    __wt_epoch((WT_SESSION_IMPL *)session, &ts);
    return ((uint64_t)ts.tv_sec * WT_MILLION + (uint64_t)ts.tv_nsec / WT_THOUSAND);
}

/*
 * testutil_pareto --
 *     Given a random value, a range and a skew percentage. Return a value between [0 and range).
 */
uint64_t
testutil_pareto(uint64_t rand, uint64_t range, u_int skew)
{
    double S1, S2, U;
#define PARETO_SHAPE 1.5

    S1 = (-1 / PARETO_SHAPE);
    S2 = range * (skew / 100.0) * (PARETO_SHAPE - 1);
    U = 1 - (double)rand / (double)UINT32_MAX;
    rand = (uint64_t)((pow(U, S1) - 1) * S2);
    /*
     * This Pareto calculation chooses out of range values about
     * 2% of the time, from my testing. That will lead to the
     * first item in the table being "hot".
     */
    if (rand > range)
        rand = 0;
    return (rand);
}

/*
 * dcalloc --
 *     Call calloc, dying on failure.
 */
void *
dcalloc(size_t number, size_t size)
{
    void *p;

    if ((p = calloc(number, size)) != NULL)
        return (p);
    testutil_die(errno, "calloc: %" WT_SIZET_FMT "B", number * size);
}

/*
 * dmalloc --
 *     Call malloc, dying on failure.
 */
void *
dmalloc(size_t len)
{
    void *p;

    if ((p = malloc(len)) != NULL)
        return (p);
    testutil_die(errno, "malloc: %" WT_SIZET_FMT "B", len);
}

/*
 * drealloc --
 *     Call realloc, dying on failure.
 */
void *
drealloc(void *p, size_t len)
{
    void *t;

    if ((t = realloc(p, len)) != NULL)
        return (t);
    testutil_die(errno, "realloc: %" WT_SIZET_FMT "B", len);
}

/*
 * dstrdup --
 *     Call strdup, dying on failure.
 */
void *
dstrdup(const void *str)
{
    char *p;

    if ((p = strdup(str)) != NULL)
        return (p);
    testutil_die(errno, "strdup");
}

/*
 * dstrndup --
 *     Call emulating strndup, dying on failure. Don't use actual strndup here as it is not
 *     supported within MSVC.
 */
void *
dstrndup(const char *str, size_t len)
{
    char *p;

    p = dcalloc(len + 1, sizeof(char));
    memcpy(p, str, len);
    return (p);
}

/*
 * example_setup --
 *     Set the program name, create a home directory for the example programs.
 */
const char *
example_setup(int argc, char *const *argv)
{
    const char *home;

    (void)argc; /* Unused variable */

    (void)testutil_set_progname(argv);

    /*
     * Create a clean test directory for this run of the test program if the environment variable
     * isn't already set (as is done by make check).
     */
    if ((home = getenv("WIREDTIGER_HOME")) == NULL)
        home = "WT_HOME";
    testutil_recreate_dir(home);
    return (home);
}

/*
 * is_mounted --
 *     Check whether the given directory (other than /) is mounted. Works only on Linux.
 */
bool
is_mounted(const char *mount_dir)
{
#ifndef __linux__
    WT_UNUSED(mount_dir);
    return false;
#else
    struct stat sb, parent_sb;
    char buf[PATH_MAX];

    testutil_snprintf(buf, sizeof(buf), "%s", mount_dir);
    testutil_assert_errno(stat(mount_dir, &sb) == 0);
    testutil_assert_errno(stat(dirname(buf), &parent_sb) == 0);

    return sb.st_dev != parent_sb.st_dev;
#endif
}

/*
 * testutil_system_internal --
 *     A convenience function that combines snprintf, system, and testutil_check.
 */
void
testutil_system_internal(const char *function, uint32_t line, const char *fmt, ...)
  WT_GCC_FUNC_ATTRIBUTE((format(printf, 2, 3)))
{
    WT_DECL_RET;
    size_t len;
    char buf[4096];
    va_list ap;

    len = 0;

    va_start(ap, fmt);
    ret = __wt_vsnprintf_len_incr(buf, sizeof(buf), &len, fmt, ap);
    va_end(ap);

    testutil_check(ret);

    if (len >= sizeof(buf))
        testutil_die(ERANGE, "The command is too long.");

    if ((ret = (system(buf))) != 0)
        testutil_die(ret, "%s/%d: system(%s)", function, line, buf);
}
