/**
 * Test that verifies the TenantID prefix the namespace in slow query logs.
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";

const rst = new ReplSetTest({
    nodes: 1,
    nodeOptions: {
        auth: '',
        setParameter: {
            multitenancySupport: true,
            featureFlagSecurityToken: true,
        },
        useLogFiles: true
    }
});
rst.startSet({keyFile: 'jstests/libs/key1'});
rst.initiate();

const kTenant = ObjectId();
const adminDb = rst.getPrimary().getDB('admin');

// Create a user for testing
assert.commandWorked(adminDb.runCommand({createUser: 'admin', pwd: 'pwd', roles: ['root']}));
assert(adminDb.auth('admin', 'pwd'));

// Set logLevel to 1 so that all queries will be logged.
assert.commandWorked(adminDb.setLogLevel(1));

// Set slow threshold to -1 to ensure that all operations are logged as SLOW.
assert.commandWorked(adminDb.setProfilingLevel(2, {slowms: -1}));

const primary = rst.getPrimary();

primary._setSecurityToken(_createTenantToken({tenant: kTenant}));
assert.commandWorked(
    primary.getDB("test").runCommand({insert: "foo", documents: [{_id: 0, a: 1, b: 1}]}));

print(`Checking ${primary.fullOptions.logFile} for client metadata message`);
const log = cat(primary.fullOptions.logFile);

const predicate =
    new RegExp(`Slow query.*"${kTenant.str}_test.foo.*"appName":"MongoDB Shell".*"command":` +
               `{"insert":"foo",`);

// Dump the log line by line to avoid log truncation
for (var a of log.split("\n")) {
    print("LOG_FILE_ENTRY: " + a);
}

assert(predicate.test(log),
       "'Slow query' log line missing in mongod log file!\n" +
           "Log file contents: " + rst.getPrimary().fullOptions.logFile);

primary._setSecurityToken(undefined);
rst.stopSet();
