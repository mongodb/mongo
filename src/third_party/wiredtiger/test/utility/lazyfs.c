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

#ifdef __linux__
#include <sys/prctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <libgen.h>
#include <signal.h>
#endif

#ifdef __linux__
#define LAZYFS_PATH "../../../lazyfs/src/lazyfs/lazyfs"
#endif

#define LAZYFS_SUFFIX "_lazyfs"

#ifdef __linux__
static char lazyfs_home[PATH_MAX]; /* Path to LazyFS */
#endif

/*
 * Deal with compiler errors due to the following functions not returning outside of Linux.
 */
#ifndef __linux__
void lazyfs_init(void) WT_GCC_FUNC_DECL_ATTRIBUTE((noreturn));
pid_t lazyfs_mount(const char *, const char *, const char *) WT_GCC_FUNC_DECL_ATTRIBUTE((noreturn));
void lazyfs_unmount(const char *, pid_t) WT_GCC_FUNC_DECL_ATTRIBUTE((noreturn));
void testutil_lazyfs_setup(WT_LAZY_FS *, const char *) WT_GCC_FUNC_DECL_ATTRIBUTE((noreturn));
void testutil_lazyfs_cleanup(WT_LAZY_FS *) WT_GCC_FUNC_DECL_ATTRIBUTE((noreturn));
#endif

/*
 * lazyfs_is_implicitly_enabled --
 *     Check whether LazyFS is implicitly enabled through being the executable name.
 */
bool
lazyfs_is_implicitly_enabled(void)
{
    size_t len;

    if (progname == NULL)
        return false;

    len = strlen(progname);
    if (len < strlen(LAZYFS_SUFFIX))
        return false;

    return strcmp(progname + (len - strlen(LAZYFS_SUFFIX)), LAZYFS_SUFFIX) == 0;
}

/*
 * lazyfs_init --
 *     Initialize the use of LazyFS for this program.
 */
void
lazyfs_init(void)
{
#ifndef __linux__
    testutil_die(ENOENT, "LazyFS is not available on this platform.");
#else
    struct stat sb;
    char buf[PATH_MAX];
    char program_dir[PATH_MAX];

    /* Find LazyFS relative to the path to the current executable. */
    testutil_assert_errno(readlink("/proc/self/exe", buf, sizeof(buf)) >= 0);
    testutil_check(__wt_snprintf(program_dir, sizeof(program_dir), "%s", dirname(buf)));

    testutil_check(__wt_snprintf(lazyfs_home, sizeof(lazyfs_home), "%s/" LAZYFS_PATH, program_dir));
    if (stat(lazyfs_home, &sb) != 0)
        testutil_die(errno, "Cannot find LazyFS.");
#endif
}

/*
 * lazyfs_create_config --
 *     Create the config file for LazyFS. Note that the path to the FIFO file must be absolute.
 */
void
lazyfs_create_config(
  const char *lazyfs_config, const char *lazyfs_control, const char *lazyfs_log_file)
{
    FILE *config_fh;

    if ((config_fh = fopen(lazyfs_config, "w")) == NULL)
        testutil_die(errno, "Cannot create LazyFS's config file.");

    testutil_assert_errno(fprintf(config_fh, "[faults]\n") >= 0);
    testutil_assert_errno(fprintf(config_fh, "fifo_path=\"%s\"\n", lazyfs_control) >= 0);

    testutil_assert_errno(fprintf(config_fh, "\n[cache]\n") >= 0);
    testutil_assert_errno(fprintf(config_fh, "apply_eviction=false\n") >= 0);

    testutil_assert_errno(fprintf(config_fh, "\n[cache.simple]\n") >= 0);
    testutil_assert_errno(fprintf(config_fh, "custom_size=\"1gb\"\n") >= 0);
    testutil_assert_errno(fprintf(config_fh, "blocks_per_page=1\n") >= 0);

    testutil_assert_errno(fprintf(config_fh, "\n[filesystem]\n") >= 0);
    testutil_assert_errno(fprintf(config_fh, "log_all_operations=false\n") >= 0);
    testutil_assert_errno(fprintf(config_fh, "logfile=\"%s\"\n", lazyfs_log_file) >= 0);

    testutil_check(fclose(config_fh));
}

/*
 * lazyfs_mount --
 *     Mount LazyFS. Note that the passed paths must be absolute.
 */
pid_t
lazyfs_mount(const char *mount_dir, const char *base_dir, const char *lazyfs_config)
{
#ifndef __linux__
    WT_UNUSED(mount_dir);
    WT_UNUSED(base_dir);
    WT_UNUSED(lazyfs_config);
    testutil_die(ENOENT, "LazyFS is not available on this platform.");
#else
    char subdir_arg[PATH_MAX];
    int e, count;
    pid_t p, parent_pid, pid;

    /*
     * Mount in a separate process that will be automatically killed if the parent unexpectedly
     * exits. In this way, we never forget to unmount.
     */

    testutil_check(__wt_snprintf(subdir_arg, sizeof(subdir_arg), "subdir=%s", base_dir));

    parent_pid = getpid();
    testutil_assert_errno((pid = fork()) >= 0);

    if (pid == 0) { /* child */
        if (prctl(PR_SET_PDEATHSIG, SIGTERM) != 0) {
            e = errno;
            kill(parent_pid, SIGTERM);
            testutil_die(e, "Failed to set up PR_SET_PDEATHSIG");
        }

        if (chdir(lazyfs_home) != 0) {
            e = errno;
            kill(parent_pid, SIGTERM);
            testutil_die(e, "Failed to change directory to LazyFS's home");
        }

        /*
         * Note that we need to call the executable directly, not via the mount script, because
         * there is otherwise no easy way to kill it automatically if the parent process suddenly
         * exits.
         */
        if (execl("./build/lazyfs", "lazyfs", mount_dir, "--config-path", lazyfs_config, "-o",
              "allow_other", "-o", "modules=subdir", "-o", subdir_arg, "-f", NULL) != 0) {
            e = errno;
            kill(parent_pid, SIGTERM);
            testutil_die(e, "Failed to start LazyFS");
        }

        /* NOTREACHED */
    }

    /* Parent. */
    if (pid < 0)
        testutil_die(errno, "Failed to start LazyFS on `%s`", mount_dir);

    /* Wait for the mount to finish. */
    count = 0;
    for (;;) {
        sleep(2);
        if (is_mounted(mount_dir))
            break;
        if (++count >= 10)
            testutil_die(ETIMEDOUT, "Failed to mount LazyFS on `%s`", mount_dir);
    }

    /* Check on the child process. */
    testutil_assert_errno((p = waitpid(pid, &e, WNOHANG)) >= 0);
    if (p > 0)
        testutil_die(ECHILD, "Failed to mount LazyFS on `%s`: Process exited with status %d",
          mount_dir, WEXITSTATUS(e));

    return pid;
#endif
}

/*
 * lazyfs_unmount --
 *     Unmount LazyFS if it is mounted. If lazyfs_pid > 0, wait for the subprocess to exit.
 */
void
lazyfs_unmount(const char *mount_dir, pid_t lazyfs_pid)
{
#ifndef __linux__
    WT_UNUSED(mount_dir);
    WT_UNUSED(lazyfs_pid);
    testutil_die(ENOENT, "LazyFS is not available on this platform.");
#else
    struct stat sb;
    int status;
    char buf[PATH_MAX];

    /* Check whether the file system is mounted. */
    if (stat(mount_dir, &sb) != 0) {
        if (errno == ENOENT)
            return; /* It's ok if the mount point doesn't exist. */
        testutil_die(errno, "Error while accessing the LazyFS mount point.");
    }
    if (!is_mounted(mount_dir))
        return;

    /* Unmount. */
    testutil_check(__wt_snprintf(
      buf, sizeof(buf), "cd '%s' && ./scripts/umount-lazyfs.sh -m \"%s\"", lazyfs_home, mount_dir));
    testutil_check(system(buf));
    if (lazyfs_pid > 0)
        testutil_assert_errno(waitpid(lazyfs_pid, &status, 0) >= 0);
#endif
}

/*
 * lazyfs_command --
 *     Run a LazyFS command.
 */
void
lazyfs_command(const char *lazyfs_control, const char *command)
{
    FILE *f;

    if ((f = fopen(lazyfs_control, "w")) == NULL)
        testutil_die(errno, "Cannot open LazyFS's control file.");

    testutil_assert_errno(fprintf(f, "lazyfs::%s\n", command) >= 0);
    testutil_check(fclose(f));
    usleep(500 * WT_THOUSAND);
}

/*
 * lazyfs_clear_cache --
 *     Clear cache.
 */
void
lazyfs_clear_cache(const char *lazyfs_control)
{
    lazyfs_command(lazyfs_control, "clear-cache");
}

/*
 * lazyfs_display_cache_usage --
 *     Display cache usage.
 */
void
lazyfs_display_cache_usage(const char *lazyfs_control)
{
    lazyfs_command(lazyfs_control, "display-cache-usage");
}

/*
 * testutil_lazyfs_setup --
 *     Set up LazyFS for the test. Note that the home directory must already exist.
 */
void
testutil_lazyfs_setup(WT_LAZY_FS *lazyfs, const char *home)
{
#ifndef __linux__
    WT_UNUSED(lazyfs);
    WT_UNUSED(home);
    testutil_die(ENOENT, "LazyFS is not available on this platform.");
#else
    char home_canonical[PATH_MAX];
    char *str;

    memset(lazyfs, 0, sizeof(*lazyfs));

    /* Initialize LazyFS for the application. */
    lazyfs_init();

    /* Get the canonical path to the home directory. */
    testutil_assert_errno((str = canonicalize_file_name(home)) != NULL);
    testutil_check(__wt_snprintf(home_canonical, sizeof(home_canonical), "%s", str));
    free(str);

    /* Create the base directory on the underlying file system. */
    testutil_check(
      __wt_snprintf(lazyfs->base, sizeof(lazyfs->base), "%s/%s", home_canonical, LAZYFS_BASE_DIR));
    testutil_make_work_dir(lazyfs->base);

    /* Set up the relevant LazyFS files. */
    testutil_check(__wt_snprintf(
      lazyfs->control, sizeof(lazyfs->control), "%s/%s", home_canonical, LAZYFS_CONTROL_FILE));
    testutil_check(__wt_snprintf(
      lazyfs->config, sizeof(lazyfs->config), "%s/%s", home_canonical, LAZYFS_CONFIG_FILE));
    testutil_check(__wt_snprintf(
      lazyfs->logfile, sizeof(lazyfs->logfile), "%s/%s", home_canonical, LAZYFS_LOG_FILE));
    lazyfs_create_config(lazyfs->config, lazyfs->control, lazyfs->logfile);

    /* Mount LazyFS. */
    testutil_check(__wt_snprintf(
      lazyfs->mountpoint, sizeof(lazyfs->mountpoint), "%s/%s", home_canonical, WT_HOME_DIR));
    lazyfs->pid = lazyfs_mount(lazyfs->mountpoint, lazyfs->base, lazyfs->config);
#endif
}

/*
 * testutil_lazyfs_clear_cache --
 *     Clear the cache. Also print the cache usage to the log for debugging purposes.
 */
void
testutil_lazyfs_clear_cache(WT_LAZY_FS *lazyfs)
{
    lazyfs_display_cache_usage(lazyfs->control);
    lazyfs_clear_cache(lazyfs->control);
}

/*
 * testutil_lazyfs_cleanup --
 *     Clean up LazyFS: Unmount the file system and delete the (now empty) working directory.
 */
void
testutil_lazyfs_cleanup(WT_LAZY_FS *lazyfs)
{
#ifndef __linux__
    WT_UNUSED(lazyfs);
    testutil_die(ENOENT, "LazyFS is not available on this platform.");
#else
    lazyfs_unmount(lazyfs->mountpoint, lazyfs->pid);
    lazyfs->pid = 0;

    testutil_clean_work_dir(lazyfs->mountpoint);
#endif
}
