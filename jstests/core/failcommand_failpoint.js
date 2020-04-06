/* Tests the "failCommand" failpoint.
 * @tags: [assumes_read_concern_unchanged, assumes_read_preference_unchanged, requires_fcv_44]
 */
(function() {
"use strict";

load("jstests/libs/fixture_helpers.js");
load("jstests/libs/retryable_writes_util.js");

const testDB = db.getSiblingDB("test_failcommand");
const adminDB = db.getSiblingDB("admin");

const getCurOpMetadata = function() {
    let myUri = adminDB.runCommand({whatsmyuri: 1}).you;
    return adminDB.aggregate([{$currentOp: {localOps: true}}, {$match: {client: myUri}}])
        .toArray()[0];
};
const getThreadName = function() {
    return getCurOpMetadata().desc;
};
const getAppName = function() {
    return getCurOpMetadata().appName;
};

let threadName = getThreadName();
const appName = getAppName();

// Test idempotent configureFailPoint.
assert.commandWorked(adminDB.runCommand({
    configureFailPoint: "failCommand",
    mode: "alwaysOn",
    data: {
        errorCode: ErrorCodes.NotMaster,
        failCommands: ["ping"],
        threadName: threadName,
    }
}));
assert.commandFailedWithCode(testDB.runCommand({ping: 1}), ErrorCodes.NotMaster);
// Configure failCommand again and verify that it still works correctly.
assert.commandWorked(adminDB.runCommand({
    configureFailPoint: "failCommand",
    mode: "alwaysOn",
    data: {
        errorCode: ErrorCodes.NotMaster,
        failCommands: ["ping"],
        threadName: threadName,
    }
}));
assert.commandFailedWithCode(testDB.runCommand({ping: 1}), ErrorCodes.NotMaster);
assert.commandWorked(adminDB.runCommand({configureFailPoint: "failCommand", mode: "off"}));

// Test switching command sets.
assert.commandWorked(adminDB.runCommand({
    configureFailPoint: "failCommand",
    mode: "alwaysOn",
    data: {
        errorCode: ErrorCodes.NotMaster,
        failCommands: ["ping"],
        threadName: threadName,
    }
}));
assert.commandFailedWithCode(testDB.runCommand({ping: 1}), ErrorCodes.NotMaster);
assert.commandWorked(adminDB.runCommand({
    configureFailPoint: "failCommand",
    mode: "alwaysOn",
    data: {
        errorCode: ErrorCodes.NotMaster,
        failCommands: ["isMaster"],
        threadName: threadName,
    }
}));
assert.commandWorked(testDB.runCommand({ping: 1}));
assert.commandWorked(adminDB.runCommand({configureFailPoint: "failCommand", mode: "off"}));

// Test failpoint with extraErrorInfo
assert.commandWorked(adminDB.runCommand({
    configureFailPoint: "failCommand",
    mode: "alwaysOn",
    data: {
        errorCode: ErrorCodes.CannotImplicitlyCreateCollection,
        failCommands: ["create"],
        threadName: threadName,
        errorExtraInfo: {
            "ns": "namespace",
        }
    }
}));

{
    let result = testDB.runCommand({create: "collection"});
    assert(result.ok == 0);
    assert(result.code == ErrorCodes.CannotImplicitlyCreateCollection);
    assert(result.ns == "namespace");
}
assert.commandWorked(adminDB.runCommand({configureFailPoint: "failCommand", mode: "off"}));

// Test failpoint with extraErrorInfo and no error code
assert.commandWorked(adminDB.runCommand({
    configureFailPoint: "failCommand",
    mode: "alwaysOn",
    data: {
        failCommands: ["ping"],
        threadName: threadName,
        errorExtraInfo: {
            "desc": "some description",
        }
    }
}));
assert.commandWorked(testDB.runCommand({ping: 1}));
assert.commandWorked(adminDB.runCommand({configureFailPoint: "failCommand", mode: "off"}));

// Test failpoint for command aliases
assert.commandWorked(adminDB.runCommand({
    configureFailPoint: "failCommand",
    mode: "alwaysOn",
    data: {
        errorCode: ErrorCodes.BadValue,
        failCommands: ["dropIndexes"],
        threadName: threadName,
    }
}));
assert.commandFailedWithCode(testDB.runCommand({dropIndexes: 'collection', index: '*'}),
                             ErrorCodes.BadValue);
assert.commandWorked(testDB.runCommand({buildInfo: 1}));
assert.commandFailedWithCode(testDB.runCommand({deleteIndexes: 'collection', index: '*'}),
                             ErrorCodes.BadValue);
assert.commandWorked(testDB.runCommand({buildinfo: 1}));
assert.commandWorked(adminDB.runCommand({configureFailPoint: "failCommand", mode: "off"}));

// Test failing with a particular error code.
assert.commandWorked(adminDB.runCommand({
    configureFailPoint: "failCommand",
    mode: "alwaysOn",
    data: {
        errorCode: ErrorCodes.NotMaster,
        failCommands: ["ping"],
        threadName: threadName,
    }
}));
assert.commandFailedWithCode(testDB.runCommand({ping: 1}), ErrorCodes.NotMaster);
assert.commandWorked(adminDB.runCommand({configureFailPoint: "failCommand", mode: "off"}));

// Test that only commands specified in failCommands fail.
assert.commandWorked(adminDB.runCommand({
    configureFailPoint: "failCommand",
    mode: "alwaysOn",
    data: {
        errorCode: ErrorCodes.BadValue,
        failCommands: ["ping"],
        threadName: threadName,
    }
}));
assert.commandFailedWithCode(testDB.runCommand({ping: 1}), ErrorCodes.BadValue);
assert.commandWorked(testDB.runCommand({isMaster: 1}));
assert.commandWorked(testDB.runCommand({buildinfo: 1}));
assert.commandWorked(testDB.runCommand({find: "collection"}));
assert.commandWorked(adminDB.runCommand({configureFailPoint: "failCommand", mode: "off"}));

// Test failing with multiple commands specified in failCommands.
assert.commandWorked(adminDB.runCommand({
    configureFailPoint: "failCommand",
    mode: "alwaysOn",
    data: {
        errorCode: ErrorCodes.BadValue,
        failCommands: ["ping", "isMaster"],
        threadName: threadName,
    }
}));
assert.commandFailedWithCode(testDB.runCommand({ping: 1}), ErrorCodes.BadValue);
assert.commandFailedWithCode(testDB.runCommand({isMaster: 1}), ErrorCodes.BadValue);
assert.commandWorked(adminDB.runCommand({configureFailPoint: "failCommand", mode: "off"}));

// Test skip when failing with a particular error code.
assert.commandWorked(adminDB.runCommand({
    configureFailPoint: "failCommand",
    mode: {skip: 2},
    data: {
        errorCode: ErrorCodes.NotMaster,
        failCommands: ["ping"],
        threadName: threadName,
    }
}));
assert.commandWorked(testDB.runCommand({ping: 1}));
assert.commandWorked(testDB.runCommand({ping: 1}));
assert.commandFailedWithCode(testDB.runCommand({ping: 1}), ErrorCodes.NotMaster);
assert.commandWorked(adminDB.runCommand({configureFailPoint: "failCommand", mode: "off"}));

// Test times when failing with a particular error code.
assert.commandWorked(adminDB.runCommand({
    configureFailPoint: "failCommand",
    mode: {times: 2},
    data: {
        errorCode: ErrorCodes.NotMaster,
        failCommands: ["ping"],
        threadName: threadName,
    }
}));
assert.commandFailedWithCode(testDB.runCommand({ping: 1}), ErrorCodes.NotMaster);
assert.commandFailedWithCode(testDB.runCommand({ping: 1}), ErrorCodes.NotMaster);
assert.commandWorked(testDB.runCommand({ping: 1}));
assert.commandWorked(adminDB.runCommand({configureFailPoint: "failCommand", mode: "off"}));

// Commands not specified in failCommands are not counted for skip.
assert.commandWorked(adminDB.runCommand({
    configureFailPoint: "failCommand",
    mode: {skip: 1},
    data: {
        errorCode: ErrorCodes.BadValue,
        failCommands: ["ping"],
        threadName: threadName,
    }
}));
assert.commandWorked(testDB.runCommand({isMaster: 1}));
assert.commandWorked(testDB.runCommand({buildinfo: 1}));
assert.commandWorked(testDB.runCommand({ping: 1}));
assert.commandWorked(testDB.runCommand({find: "c"}));
assert.commandFailedWithCode(testDB.runCommand({ping: 1}), ErrorCodes.BadValue);
assert.commandWorked(adminDB.runCommand({configureFailPoint: "failCommand", mode: "off"}));

// Commands not specified in failCommands are not counted for times.
assert.commandWorked(adminDB.runCommand({
    configureFailPoint: "failCommand",
    mode: {times: 1},
    data: {
        errorCode: ErrorCodes.BadValue,
        failCommands: ["ping"],
        threadName: threadName,
    }
}));
assert.commandWorked(testDB.runCommand({isMaster: 1}));
assert.commandWorked(testDB.runCommand({buildinfo: 1}));
assert.commandWorked(testDB.runCommand({find: "c"}));
assert.commandFailedWithCode(testDB.runCommand({ping: 1}), ErrorCodes.BadValue);
assert.commandWorked(testDB.runCommand({ping: 1}));
assert.commandWorked(adminDB.runCommand({configureFailPoint: "failCommand", mode: "off"}));

// Test closing connection.
assert.commandWorked(adminDB.runCommand({
    configureFailPoint: "failCommand",
    mode: "alwaysOn",
    data: {
        closeConnection: true,
        failCommands: ["ping"],
        threadName: threadName,
    }
}));
assert.throws(() => testDB.runCommand({ping: 1}));
assert.commandWorked(adminDB.runCommand({configureFailPoint: "failCommand", mode: "off"}));

threadName = getThreadName();

// Test that only commands specified in failCommands fail when closing the connection.
assert.commandWorked(adminDB.runCommand({
    configureFailPoint: "failCommand",
    mode: "alwaysOn",
    data: {
        closeConnection: true,
        failCommands: ["ping"],
        threadName: threadName,
    }
}));
assert.commandWorked(testDB.runCommand({isMaster: 1}));
assert.commandWorked(testDB.runCommand({buildinfo: 1}));
assert.commandWorked(testDB.runCommand({find: "c"}));
assert.throws(() => testDB.runCommand({ping: 1}));
assert.commandWorked(adminDB.runCommand({configureFailPoint: "failCommand", mode: "off"}));

threadName = getThreadName();

// Test skip when closing connection.
assert.commandWorked(adminDB.runCommand({
    configureFailPoint: "failCommand",
    mode: {skip: 2},
    data: {
        closeConnection: true,
        failCommands: ["ping"],
        threadName: threadName,
    }
}));
assert.commandWorked(testDB.runCommand({ping: 1}));
assert.commandWorked(testDB.runCommand({ping: 1}));
assert.throws(() => testDB.runCommand({ping: 1}));
assert.commandWorked(adminDB.runCommand({configureFailPoint: "failCommand", mode: "off"}));

threadName = getThreadName();

// Commands not specified in failCommands are not counted for skip.
assert.commandWorked(adminDB.runCommand({
    configureFailPoint: "failCommand",
    mode: {skip: 1},
    data: {
        closeConnection: true,
        failCommands: ["ping"],
        threadName: threadName,
    }
}));
assert.commandWorked(testDB.runCommand({isMaster: 1}));
assert.commandWorked(testDB.runCommand({buildinfo: 1}));
assert.commandWorked(testDB.runCommand({ping: 1}));
assert.commandWorked(testDB.runCommand({find: "c"}));
assert.throws(() => testDB.runCommand({ping: 1}));
assert.commandWorked(adminDB.runCommand({configureFailPoint: "failCommand", mode: "off"}));

threadName = getThreadName();

// Commands not specified in failCommands are not counted for times.
assert.commandWorked(adminDB.runCommand({
    configureFailPoint: "failCommand",
    mode: {times: 1},
    data: {
        closeConnection: true,
        failCommands: ["ping"],
        threadName: threadName,
    }
}));
assert.commandWorked(testDB.runCommand({isMaster: 1}));
assert.commandWorked(testDB.runCommand({buildinfo: 1}));
assert.commandWorked(testDB.runCommand({find: "c"}));
assert.throws(() => testDB.runCommand({ping: 1}));
assert.commandWorked(testDB.runCommand({ping: 1}));
assert.commandWorked(adminDB.runCommand({configureFailPoint: "failCommand", mode: "off"}));

threadName = getThreadName();

// Cannot fail on "configureFailPoint" command.
assert.commandWorked(adminDB.runCommand({
    configureFailPoint: "failCommand",
    mode: {times: 1},
    data: {
        errorCode: ErrorCodes.BadValue,
        failCommands: ["configureFailPoint"],
        threadName: threadName,
    }
}));
assert.commandWorked(adminDB.runCommand({configureFailPoint: "failCommand", mode: "off"}));

// Test with success and writeConcernError.
assert.commandWorked(adminDB.runCommand({
    configureFailPoint: "failCommand",
    mode: {times: 1},
    data: {
        writeConcernError: {code: 12345, errmsg: "hello"},
        failCommands: ['insert', 'ping'],
        threadName: threadName,
    }
}));
// Commands that don't support writeConcern don't tick counter.
assert.commandWorked(testDB.runCommand({ping: 1}));
// Unlisted commands don't tick counter.
assert.commandWorked(testDB.runCommand({update: "c", updates: [{q: {}, u: {}, upsert: true}]}));
var res = testDB.runCommand({insert: "c", documents: [{}]});
assert.commandWorkedIgnoringWriteConcernErrors(res);
assert.eq(res.writeConcernError, {code: 12345, errmsg: "hello"});
assert.commandWorked(testDB.runCommand({insert: "c", documents: [{}]}));  // Works again.
assert.commandWorked(adminDB.runCommand({configureFailPoint: "failCommand", mode: "off"}));

// Test with natural failure and writeConcernError.

// This document is removed before testing the following insert to prevent a DuplicateKeyError
// if the failcommand_failpoint test is run multiple times on the same fixture.
testDB.c.remove({_id: 'dup'});

assert.commandWorked(testDB.runCommand({insert: "c", documents: [{_id: 'dup'}]}));
assert.commandWorked(adminDB.runCommand({
    configureFailPoint: "failCommand",
    mode: {times: 1},
    data: {
        writeConcernError: {code: 12345, errmsg: "hello"},
        failCommands: ['insert'],
        threadName: threadName,
    }
}));
var res = testDB.runCommand({insert: "c", documents: [{_id: 'dup'}]});
assert.commandFailedWithCode(res, ErrorCodes.DuplicateKey);
assert.eq(res.writeConcernError, {code: 12345, errmsg: "hello"});
assert.commandWorked(testDB.runCommand({insert: "c", documents: [{}]}));  // Works again.
assert.commandWorked(adminDB.runCommand({configureFailPoint: "failCommand", mode: "off"}));

// Test that specifying both writeConcernError and closeConnection : false will not make
// `times` decrement twice per operation
assert.commandWorked(adminDB.runCommand({
    configureFailPoint: "failCommand",
    mode: {times: 2},
    data: {
        failCommands: ["insert"],
        closeConnection: false,
        writeConcernError: {code: 12345, errmsg: "hello"},
        threadName: threadName,
    }
}));

var res = testDB.runCommand({insert: "test", documents: [{a: "something"}]});
assert.commandWorkedIgnoringWriteConcernErrors(res);
assert.eq(res.writeConcernError, {code: 12345, errmsg: "hello"});
res = testDB.runCommand({insert: "test", documents: [{a: "something else"}]});
assert.commandWorkedIgnoringWriteConcernErrors(res);
assert.eq(res.writeConcernError, {code: 12345, errmsg: "hello"});
assert.commandWorked(testDB.runCommand({insert: "test", documents: [{b: "or_other"}]}));

//
// Test that the namespace parameter is obeyed.
//
assert.commandWorked(adminDB.runCommand({
    configureFailPoint: "failCommand",
    mode: {times: 1},
    data: {
        errorCode: ErrorCodes.InternalError,
        failCommands: ["find"],
        namespace: testDB.getName() + ".foo",
        threadName: threadName,
    }
}));

// A find against a different namespace should not trigger the failpoint.
assert.commandWorked(testDB.runCommand({find: "test"}));

// A find against the namespace given to the failpoint should trigger the failpoint.
assert.commandFailedWithCode(testDB.runCommand({find: "foo"}), ErrorCodes.InternalError);

//
// Test that the namespace parameter is obeyed for write concern errors.
//
assert.commandWorked(adminDB.runCommand({
    configureFailPoint: "failCommand",
    mode: {times: 1},
    data: {
        failCommands: ["insert"],
        namespace: testDB.getName() + ".foo",
        threadName: threadName,
        writeConcernError: {code: ErrorCodes.InternalError, errmsg: "foo"},
    }
}));

// An insert to a different namespace should not trigger the failpoint.
assert.commandWorked(
    testDB.runCommand({insert: "test", documents: [{x: "doc_for_namespace_no_wce"}]}));

// An insert to the namespace given to the failpoint should trigger the failpoint.
res = assert.commandWorkedIgnoringWriteConcernErrors(testDB.runCommand(
    {insert: "foo", documents: [{x: "doc_for_namespace_case_should_trigger_wce"}]}));
assert.eq(res.writeConcernError, {code: ErrorCodes.InternalError, errmsg: "foo"});

// Test failing with error labels will not make `times` decrement twice.
assert.commandWorked(adminDB.runCommand({
    configureFailPoint: "failCommand",
    mode: {times: 1},
    data: {
        errorCode: ErrorCodes.BadValue,
        failCommands: ["ping"],
        errorLabels: ["Foo", "Bar"],
        threadName: threadName
    }
}));
res = assert.commandFailedWithCode(testDB.runCommand({ping: 1}), ErrorCodes.BadValue);
assert.eq(res.errorLabels, ["Foo", "Bar"], res);
assert.commandWorked(testDB.runCommand({ping: 1}));

// Test specifying both writeConcernError and errorLabels.
assert.commandWorked(adminDB.runCommand({
    configureFailPoint: "failCommand",
    mode: {times: 1},
    data: {
        writeConcernError: {code: 12345, errmsg: "hello"},
        failCommands: ["insert"],
        errorLabels: ["Foo", "Bar"],
        threadName: threadName
    }
}));
res = testDB.runCommand({insert: "c", documents: [{}]});
assert.eq(res.writeConcernError, {code: 12345, errmsg: "hello"});
assert.eq(res.errorLabels, ["Foo", "Bar"], res);

// Test failCommand with empty errorLabels.
assert.commandWorked(adminDB.runCommand({
    configureFailPoint: "failCommand",
    mode: {times: 1},
    data: {
        errorCode: ErrorCodes.BadValue,
        failCommands: ["ping"],
        errorLabels: [],
        threadName: threadName
    }
}));
res = assert.commandFailedWithCode(testDB.runCommand({ping: 1}), ErrorCodes.BadValue);
// There should be no errorLabels field if no error labels provided in failCommand.
assert(!res.hasOwnProperty("errorLabels"));

// Test specifying both writeConcernError and empty errorLabels.
assert.commandWorked(adminDB.runCommand({
    configureFailPoint: "failCommand",
    mode: {times: 1},
    data: {
        writeConcernError: {code: 12345, errmsg: "hello"},
        failCommands: ["insert"],
        errorLabels: [],
        threadName: threadName
    }
}));
res = testDB.runCommand({insert: "c", documents: [{}]});
assert.eq(res.writeConcernError, {code: 12345, errmsg: "hello"});
// There should be no errorLabels field if no error labels provided in failCommand.
assert(!res.hasOwnProperty("errorLabels"));

// Test specifying errorLabels without errorCode or writeConcernError.
assert.commandWorked(adminDB.runCommand({
    configureFailPoint: "failCommand",
    mode: {times: 1},
    data: {failCommands: ["ping"], errorLabels: ["Foo", "Bar"], threadName: threadName}
}));
// The command should not fail if no errorCode or writeConcernError specified.
res = assert.commandWorked(testDB.runCommand({ping: 1}));
// As the command does not fail, there should not be any error labels even if errorLabels is
// specified in the failCommand.
assert(!res.hasOwnProperty("errorLabels"), res);
assert.commandWorked(adminDB.runCommand({configureFailPoint: "failCommand", mode: "off"}));

// Test support for "appName" arg to failCommand
assert.commandWorked(adminDB.runCommand({
    configureFailPoint: "failCommand",
    mode: "alwaysOn",
    data: {
        failCommands: ["ping"],
        errorCode: ErrorCodes.NotMaster,
        threadName: threadName,
        appName: appName,
    }
}));
assert.commandFailedWithCode(testDB.runCommand({ping: 1}), ErrorCodes.NotMaster);

assert.commandWorked(adminDB.runCommand({
    configureFailPoint: "failCommand",
    mode: "alwaysOn",
    data: {
        failCommands: ["ping"],
        errorCode: ErrorCodes.NotMaster,
        threadName: threadName,
        appName: "made up app name",
    }
}));
assert.commandWorked(testDB.runCommand({ping: 1}));

// Only run error labels override tests for replica set if storage engine supports document-level
// locking because the tests require retryable writes.
// And mongos doesn't return RetryableWriteError labels.
if (!FixtureHelpers.isReplSet(adminDB) ||
    !RetryableWritesUtil.storageEngineSupportsRetryableWrites(jsTest.options().storageEngine)) {
    jsTestLog("Skipping error labels override tests");
    return;
}

// Test error labels override.
assert.commandWorked(adminDB.runCommand({
    configureFailPoint: "failCommand",
    mode: {times: 1},
    data: {
        errorCode: ErrorCodes.NotMaster,
        failCommands: ["insert"],
        errorLabels: ["Foo"],
        threadName: threadName
    }
}));
// This normally fails with RetryableWriteError label.
res = assert.commandFailedWithCode(
    testDB.runCommand(
        {insert: "test", documents: [{x: "retryable_write"}], txnNumber: NumberLong(0)}),
    ErrorCodes.NotMaster);
// Test that failCommand overrides the error label to "Foo".
assert.eq(res.errorLabels, ["Foo"], res);

// Test error labels override while specifying writeConcernError.
assert.commandWorked(adminDB.runCommand({
    configureFailPoint: "failCommand",
    mode: {times: 1},
    data: {
        writeConcernError: {code: ErrorCodes.NotMaster, errmsg: "hello"},
        failCommands: ["insert"],
        errorLabels: ["Foo"],
        threadName: threadName
    }
}));
// This normally fails with RetryableWriteError label.
res = testDB.runCommand(
    {insert: "test", documents: [{x: "retryable_write"}], txnNumber: NumberLong(0)});
assert.eq(res.writeConcernError, {code: ErrorCodes.NotMaster, errmsg: "hello"});
// Test that failCommand overrides the error label to "Foo".
assert.eq(res.errorLabels, ["Foo"], res);

// Test error labels override with empty errorLabels.
assert.commandWorked(adminDB.runCommand({
    configureFailPoint: "failCommand",
    mode: {times: 1},
    data: {
        errorCode: ErrorCodes.NotMaster,
        failCommands: ["insert"],
        errorLabels: [],
        threadName: threadName
    }
}));
// This normally fails with RetryableWriteError label.
res = assert.commandFailedWithCode(
    testDB.runCommand(
        {insert: "test", documents: [{x: "retryable_write"}], txnNumber: NumberLong(0)}),
    ErrorCodes.NotMaster);
// There should be no errorLabels field if no error labels provided in failCommand.
assert(!res.hasOwnProperty("errorLabels"), res);

// Test error labels override with empty errorLabels while specifying writeConcernError.
assert.commandWorked(adminDB.runCommand({
    configureFailPoint: "failCommand",
    mode: {times: 1},
    data: {
        writeConcernError: {code: ErrorCodes.NotMaster, errmsg: "hello"},
        failCommands: ["insert"],
        errorLabels: [],
        threadName: threadName
    }
}));
// This normally fails with RetryableWriteError label.
res = testDB.runCommand(
    {insert: "test", documents: [{x: "retryable_write"}], txnNumber: NumberLong(0)});
assert.eq(res.writeConcernError, {code: ErrorCodes.NotMaster, errmsg: "hello"});
// There should be no errorLabels field if no error labels provided in failCommand.
assert(!res.hasOwnProperty("errorLabels"), res);
}());
