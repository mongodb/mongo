// Exit code that the watchdog uses on exit
const EXIT_WATCHDOG = 61;

/**
 * Control the Charybdefs file system for Fault Injectiong testing
 *
 * @param {string} test_name unique name for test directories
 */
function CharybdefsControl(test_name) {
    'use strict';

    const python = "/opt/mongodbtoolchain/v3/bin/python3";
    let control_py = "/data/charybdefs/mongo/control.py";

    // Use the minimum watchdog period
    const wd_period_sec = 60;

    // Since the watchdog can take up to (2 x period) to detect failures, stall the write for that
    // amount of time plus a small buffer of time to account for thread scheduling, etc.
    const fs_delay_sec = wd_period_sec * 2 + 5;

    const mount_point = MongoRunner.toRealPath(test_name + '_mnt');
    const backing_path = MongoRunner.toRealPath(test_name + '_backing');

    this._runControl = function(cmd, ...args) {
        let cmd_args = [python, control_py, cmd];
        cmd_args = cmd_args.concat(args);
        let ret = run.apply(null, cmd_args);
        assert.eq(ret, 0);
    };

    /**
     * Get the path of the mounted Charybdefs file system.
     *
     * @return {string} mount point
     */
    this.getMountPath = function() {
        return mount_point;
    };

    /**
     * Get the Watchdog Period.
     *
     * @return {number} number of sections
     */
    this.getWatchdogPeriodSeconds = function() {
        return wd_period_sec;
    };

    /**
     *  Start the Charybdefs filesystem.
     */
    this.start = function() {
        this.cleanup();

        this._runControl("start",
                         "--fuse_mount=" + mount_point,
                         "--backing_path=" + backing_path,
                         "--log_file=foo_fs.log");
        print("Charybdefs sucessfully started.");
    };

    // Get the current check generation
    function _getGeneration(admin) {
        const result = admin.runCommand({"serverStatus": 1});

        assert.commandWorked(result);

        return result.watchdog.checkGeneration;
    }

    /**
     *  Wait for the watchdog to run some checks first.
     *
     * @param {object} MongoDB connection to admin database
     */
    this.waitForWatchdogToStart = function(admin) {
        print("Waiting for MongoDB watchdog to checks run twice.");
        assert.soon(function() {
            return _getGeneration(admin) > 2;
        }, "Watchdog did not start running", 5 * wd_period_sec * 1000);
    };

    /**
     *  Inject delay on write, and wait to MongoDB to get hung.
     *
     * @param {string} file_name - file name to inject fault on
     */
    this.addWriteDelayFaultAndWait = function(file_name) {
        // Convert seconds to microseconds for charybdefs
        const delay_us = fs_delay_sec * 1000000;
        this.addFault("write_buf", file_name, delay_us);

        // Wait for watchdog to stop
        print("Waiting for MongoDB to hang.");
        sleep(fs_delay_sec * 1000);

    };

    /**
     * Add a fault to inject.
     *
     * @param {string} method - name of fuse method to inject fault for
     * @param {string} file_name - file name to inject fault on
     * @param {number} delay_us - optional delay in microseconds to wait
     */
    this.addFault = function(method, file_name, delay_us) {

        this._runControl("set_fault",
                         "--methods=" + method,
                         "--errno=5",
                         "--probability=100000",
                         "--regexp=.*" + file_name,
                         "--delay_us=" + delay_us);
    };

    /**
     * Shutdown and clean up the Charybdefs filesystem.
     */
    this.cleanup = function() {
        this._runControl("stop_all", "--fuse_mount=" + mount_point);

        // Delete any remaining files
        resetDbpath(mount_point);
        resetDbpath(backing_path);
    };
}
