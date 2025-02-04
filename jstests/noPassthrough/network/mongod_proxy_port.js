/**
 * Tests the mongod proxyPort CLI argument
 * @tags: [
 *   requires_fcv_81,
 * ]
 */

const kProxyPortConflictLogId = 9967800;

function startWithProxyPort(port, proxy) {
    const conn = MongoRunner.runMongod({port: port, 'proxyPort': proxy});
    assert.commandWorked(conn.getDB("admin").runCommand({ping: 1}));
    MongoRunner.stopMongod(conn);
}

function listenerPortConflict(port, proxy) {
    const res = MongoRunner.runMongod(
        {port: port, waitForConnect: false, 'proxyPort': proxy, useLogFiles: true});
    assert.soon(function() {
        try {
            let ret = checkProgram(res.pid);
            return (ret.alive == false && ret.exitCode == 2);
        } catch (e) {
            return false;
        }
    });

    let hasPortError = false;
    cat(res.fullOptions.logFile).trim().split("\n").forEach((line) => {
        const entry = JSON.parse(line);
        if (entry.id == kProxyPortConflictLogId) {
            assert.eq(entry.attr.port, proxy);
            hasPortError = true;
        }
    });
    assert(hasPortError);
}

const listener = allocatePort();
const proxy = allocatePort();

startWithProxyPort(listener, proxy);
listenerPortConflict(listener, listener);
