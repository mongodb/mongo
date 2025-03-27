
function logClusterPorts(st) {
    st.printNodes();
}
function isTimeToWake(waitFor) {
    if (waitFor.hasOwnProperty("anyDataIn")) {
        if (waitFor.anyDataIn.findOne() != null) {
            return true;
        } else {
            jsTestLog(`Put data in ${
                waitFor.anyDataIn.getFullName()} collection to break this infinite loop.`);
            return false;
        }
    } else {
        jsTestLog("Was not given a termination condition. Will keep looping forever.");
    }
    return false;
}

/**
 * Infinite loops and periodically logs information about the cluster. The intended use is to block
 * your test at some interesting point so that you can attach via gdb to one of the nodes. The
 * infinite loop can be broken/unblocked if you insert data into a collection which you've named in
 * 'opts', like so:
 *
 * hangTestToAttachGDB(st, {waitFor: {anyDataIn: testDB["SENTINEL"]}});
 *
 * Once you have your test hanging, you can use the 'ps' tool to find the process id (pid) of the
 * target node, and then do something like:
 * /opt/mongodbtoolchain/v4/bin/gdb bazel-bin/install/bin/mongod
 * ...
 * (gdb prompt) attach <pid>
 *
 * @param {ShardingTest} st - For printing out cluster info - making it easier to identify which
 *     node to connect gdb to.
 * @param {Object} opts - How to break out of the loop. Only supported option right now is
 *     'waitFor.anyDataIn' which should be a collection object (not a string).
 */
export function hangTestToAttachGDB(st, opts) {
    const portLogIntervalMS = 10000;
    const intervalMS = 1000;
    let iters = 0;
    while (true) {
        sleep(intervalMS);
        ++iters;
        if (intervalMS * iters >= portLogIntervalMS) {
            jsTestLog("Here are the ports to connect to");
            logClusterPorts(st);
        }
        jsTestLog("Test is sleeping waiting for you to connect");
        if (opts.waitFor && isTimeToWake(opts.waitFor)) {
            jsTestLog("Breaking sleep loop");
            break;
        }
    }
}
