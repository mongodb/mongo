/* Tests the "failCommand" failpoint.
 * @tags: [assumes_read_concern_unchanged, assumes_read_preference_unchanged]
 */
(function() {
    "use strict";

    const testDB = db.getSiblingDB("test_failcommand");
    const adminDB = db.getSiblingDB("admin");

    // Test failing with a particular error code.
    assert.commandWorked(adminDB.runCommand({
        configureFailPoint: "failCommand",
        mode: "alwaysOn",
        data: {errorCode: ErrorCodes.NotMaster, failCommands: ["find"]}
    }));
    assert.commandFailedWithCode(testDB.runCommand({find: "c"}), ErrorCodes.NotMaster);
    assert.commandWorked(adminDB.runCommand({configureFailPoint: "failCommand", mode: "off"}));

    // Test that only commands specified in failCommands fail.
    assert.commandWorked(adminDB.runCommand({
        configureFailPoint: "failCommand",
        mode: "alwaysOn",
        data: {errorCode: ErrorCodes.BadValue, failCommands: ["find"]}
    }));
    assert.commandFailedWithCode(testDB.runCommand({find: "c"}), ErrorCodes.BadValue);
    assert.commandWorked(testDB.runCommand({isMaster: 1}));
    assert.commandWorked(testDB.runCommand({buildinfo: 1}));
    assert.commandWorked(testDB.runCommand({ping: 1}));
    assert.commandWorked(adminDB.runCommand({configureFailPoint: "failCommand", mode: "off"}));

    // Test failing with multiple commands specified in failCommands.
    assert.commandWorked(adminDB.runCommand({
        configureFailPoint: "failCommand",
        mode: "alwaysOn",
        data: {errorCode: ErrorCodes.BadValue, failCommands: ["find", "isMaster"]}
    }));
    assert.commandFailedWithCode(testDB.runCommand({find: "c"}), ErrorCodes.BadValue);
    assert.commandFailedWithCode(testDB.runCommand({isMaster: 1}), ErrorCodes.BadValue);
    assert.commandWorked(adminDB.runCommand({configureFailPoint: "failCommand", mode: "off"}));

    // Test skip when failing with a particular error code.
    assert.commandWorked(adminDB.runCommand({
        configureFailPoint: "failCommand",
        mode: {skip: 2},
        data: {errorCode: ErrorCodes.NotMaster, failCommands: ["find"]}
    }));
    assert.commandWorked(testDB.runCommand({find: "c"}));
    assert.commandWorked(testDB.runCommand({find: "c"}));
    assert.commandFailedWithCode(testDB.runCommand({find: "c"}), ErrorCodes.NotMaster);
    assert.commandWorked(adminDB.runCommand({configureFailPoint: "failCommand", mode: "off"}));

    // Test times when failing with a particular error code.
    assert.commandWorked(adminDB.runCommand({
        configureFailPoint: "failCommand",
        mode: {times: 2},
        data: {errorCode: ErrorCodes.NotMaster, failCommands: ["find"]}
    }));
    assert.commandFailedWithCode(testDB.runCommand({find: "c"}), ErrorCodes.NotMaster);
    assert.commandFailedWithCode(testDB.runCommand({find: "c"}), ErrorCodes.NotMaster);
    assert.commandWorked(testDB.runCommand({find: "c"}));
    assert.commandWorked(adminDB.runCommand({configureFailPoint: "failCommand", mode: "off"}));

    // Commands not specified in failCommands are not counted for skip.
    assert.commandWorked(adminDB.runCommand({
        configureFailPoint: "failCommand",
        mode: {skip: 1},
        data: {errorCode: ErrorCodes.BadValue, failCommands: ["find"]}
    }));
    assert.commandWorked(testDB.runCommand({isMaster: 1}));
    assert.commandWorked(testDB.runCommand({buildinfo: 1}));
    assert.commandWorked(testDB.runCommand({ping: 1}));
    assert.commandWorked(testDB.runCommand({find: "c"}));
    assert.commandFailedWithCode(testDB.runCommand({find: "c"}), ErrorCodes.BadValue);
    assert.commandWorked(adminDB.runCommand({configureFailPoint: "failCommand", mode: "off"}));

    // Commands not specified in failCommands are not counted for times.
    assert.commandWorked(adminDB.runCommand({
        configureFailPoint: "failCommand",
        mode: {times: 1},
        data: {errorCode: ErrorCodes.BadValue, failCommands: ["find"]}
    }));
    assert.commandWorked(testDB.runCommand({isMaster: 1}));
    assert.commandWorked(testDB.runCommand({buildinfo: 1}));
    assert.commandWorked(testDB.runCommand({ping: 1}));
    assert.commandFailedWithCode(testDB.runCommand({find: "c"}), ErrorCodes.BadValue);
    assert.commandWorked(testDB.runCommand({find: "c"}));
    assert.commandWorked(adminDB.runCommand({configureFailPoint: "failCommand", mode: "off"}));

    // Test closing connection.
    assert.commandWorked(adminDB.runCommand({
        configureFailPoint: "failCommand",
        mode: "alwaysOn",
        data: {closeConnection: true, failCommands: ["find"]}
    }));
    assert.throws(() => testDB.runCommand({find: "c"}));
    assert.commandWorked(adminDB.runCommand({configureFailPoint: "failCommand", mode: "off"}));

    // Test that only commands specified in failCommands fail when closing the connection.
    assert.commandWorked(adminDB.runCommand({
        configureFailPoint: "failCommand",
        mode: "alwaysOn",
        data: {closeConnection: true, failCommands: ["find"]}
    }));
    assert.commandWorked(testDB.runCommand({isMaster: 1}));
    assert.commandWorked(testDB.runCommand({buildinfo: 1}));
    assert.commandWorked(testDB.runCommand({ping: 1}));
    assert.throws(() => testDB.runCommand({find: "c"}));
    assert.commandWorked(adminDB.runCommand({configureFailPoint: "failCommand", mode: "off"}));

    // Test skip when closing connection.
    assert.commandWorked(adminDB.runCommand({
        configureFailPoint: "failCommand",
        mode: {skip: 2},
        data: {closeConnection: true, failCommands: ["find"]}
    }));
    assert.commandWorked(testDB.runCommand({find: "c"}));
    assert.commandWorked(testDB.runCommand({find: "c"}));
    assert.throws(() => testDB.runCommand({find: "c"}));
    assert.commandWorked(adminDB.runCommand({configureFailPoint: "failCommand", mode: "off"}));

    // Test times when closing connection.
    assert.commandWorked(adminDB.runCommand({
        configureFailPoint: "failCommand",
        mode: {times: 2},
        data: {closeConnection: true, failCommands: ["find"]}
    }));
    assert.throws(() => testDB.runCommand({find: "c"}));
    assert.throws(() => testDB.runCommand({find: "c"}));
    assert.commandWorked(testDB.runCommand({find: "c"}));
    assert.commandWorked(adminDB.runCommand({configureFailPoint: "failCommand", mode: "off"}));

    // Commands not specified in failCommands are not counted for skip.
    assert.commandWorked(adminDB.runCommand({
        configureFailPoint: "failCommand",
        mode: {skip: 1},
        data: {closeConnection: true, failCommands: ["find"]}
    }));
    assert.commandWorked(testDB.runCommand({isMaster: 1}));
    assert.commandWorked(testDB.runCommand({buildinfo: 1}));
    assert.commandWorked(testDB.runCommand({ping: 1}));
    assert.commandWorked(testDB.runCommand({find: "c"}));
    assert.throws(() => testDB.runCommand({find: "c"}));
    assert.commandWorked(adminDB.runCommand({configureFailPoint: "failCommand", mode: "off"}));

    // Commands not specified in failCommands are not counted for times.
    assert.commandWorked(adminDB.runCommand({
        configureFailPoint: "failCommand",
        mode: {times: 1},
        data: {closeConnection: true, failCommands: ["find"]}
    }));
    assert.commandWorked(testDB.runCommand({isMaster: 1}));
    assert.commandWorked(testDB.runCommand({buildinfo: 1}));
    assert.commandWorked(testDB.runCommand({ping: 1}));
    assert.throws(() => testDB.runCommand({find: "c"}));
    assert.commandWorked(testDB.runCommand({find: "c"}));
    assert.commandWorked(adminDB.runCommand({configureFailPoint: "failCommand", mode: "off"}));

    // Cannot fail on "configureFailPoint" command.
    assert.commandWorked(adminDB.runCommand({
        configureFailPoint: "failCommand",
        mode: {times: 1},
        data: {errorCode: ErrorCodes.BadValue, failCommands: ["configureFailPoint"]}
    }));
    assert.commandWorked(adminDB.runCommand({configureFailPoint: "failCommand", mode: "off"}));

    // Test with success and writeConcernError.
    assert.commandWorked(adminDB.runCommand({
        configureFailPoint: "failCommand",
        mode: {times: 1},
        data: {
            writeConcernError: {code: 12345, errmsg: "hello"},
            failCommands: ['insert', 'find'],
        }
    }));
    // Commands that don't support writeConcern don't tick counter.
    assert.commandWorked(testDB.runCommand({find: "c"}));
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
        }
    }));
    var res = testDB.runCommand({insert: "c", documents: [{_id: 'dup'}]});
    assert.commandFailedWithCode(res, ErrorCodes.DuplicateKey);
    assert.eq(res.writeConcernError, {code: 12345, errmsg: "hello"});
    assert.commandWorked(testDB.runCommand({insert: "c", documents: [{}]}));  // Works again.
    assert.commandWorked(adminDB.runCommand({configureFailPoint: "failCommand", mode: "off"}));
}());
