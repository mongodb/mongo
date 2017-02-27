// This test addresses the invalid namespace issue encountered in SERVER-26440, and checks the other
// explainable commands to ensure none are affected by the same bug.

(function() {
    "use strict";

    // Start the sharding test.
    let st = new ShardingTest({mongos: 1, shards: 1});

    // Enable sharding on the "test" database.
    assert.commandWorked(st.s.adminCommand({enableSharding: "test"}));

    // Connect to the "test" database.
    let testDB = st.s.getDB("test");

    // Create a non-empty test collection.
    assert.writeOK(testDB.server26440.insert({a: 1}));

    // Test explain of aggregate fails with an invalid collection name.
    // TODO SERVER-24128: The following two commands fail as defined in non-auth configurations,
    // but fail differently in auth configurations. We should fail identically in both
    // configurations.
    // assert.commandFailedWithCode(testDB.runCommand({aggregate: "", pipeline:[], explain:true}),
    //                              ErrorCodes.UnknownError);
    // assert.commandFailedWithCode(testDB.runCommand({aggregate: "\0", pipeline:[], explain:true}),
    //                              ErrorCodes.UnknownError);
    // TODO SERVER-24128: Make namespace parsing for group reject names with embedded null bytes.
    // TODO SERVER-24128: The following command runs successfully in non-auth configurations, but
    // fails in auth configurations. We should fail in both configurations.
    // assert.commandFailedWithCode(testDB.runCommand({aggregate: "a\0b", pipeline:[],
    // explain:true}), ErrorCodes.InvalidNamespace);

    // Test explain of count fails with an invalid collection name.
    assert.commandFailedWithCode(testDB.runCommand({explain: {count: ""}}),
                                 ErrorCodes.InvalidNamespace);
    assert.commandFailedWithCode(testDB.runCommand({explain: {count: "\0"}}),
                                 ErrorCodes.InvalidNamespace);
    assert.commandFailedWithCode(testDB.runCommand({explain: {count: "a\0b"}}), 17295);

    // Test explain of distinct fails with an invalid collection name.
    assert.commandFailedWithCode(testDB.runCommand({explain: {distinct: "", key: "a"}}),
                                 ErrorCodes.InvalidNamespace);
    assert.commandFailedWithCode(testDB.runCommand({explain: {distinct: "\0", key: "a"}}), 17295);
    assert.commandFailedWithCode(testDB.runCommand({explain: {distinct: "a\0b", key: "a"}}), 17295);

    // Test explain of group fails with an invalid collection name.
    // TODO SERVER-24128: Currently, we massert() and print a stack trace. Instead, we should fail
    // gracefully with a user assertion.
    // TODO SERVER-24128: The following command fails in non-auth configurations, but fails
    // differently in auth configurations. We should fail identically in both configurations.
    // assert.commandFailedWithCode(
    //    testDB.runCommand({explain: {group: {ns: "", $reduce: () => {}, initial: {}}}}),
    //    ErrorCodes.InvalidNamespace);
    // TODO SERVER-24128: Make namespace parsing for explain of group reject names with embedded
    // null bytes.
    // TODO SERVER-24128: The following two commands run successfully in non-auth configurations,
    // but fail in auth configurations. We should fail in both configurations.
    // assert.commandFailedWithCode(
    //    testDB.runCommand({explain: {group: {ns: "\0", $reduce: () => {}, initial: {}}}}),
    //    ErrorCodes.InvalidNamespace);
    // TODO SERVER-24128: Make namespace parsing for explain of group reject names with embedded
    // null bytes.
    // assert.commandFailedWithCode(
    //    testDB.runCommand({explain: {group: {ns: "a\0b", $reduce: () => {}, initial: {}}}}),
    //    ErrorCodes.InvalidNamespace);

    // Test explain of find fails with an invalid collection name.
    assert.commandFailedWithCode(testDB.runCommand({explain: {find: ""}}),
                                 ErrorCodes.InvalidNamespace);
    assert.commandFailedWithCode(testDB.runCommand({explain: {find: "\0"}}),
                                 ErrorCodes.InvalidNamespace);
    // TODO SERVER-24128: Make namespace parsing for explain of find reject names with embedded null
    // bytes.
    // assert.commandFailedWithCode(testDB.runCommand({explain: {find: "a\0b"}}),
    // ErrorCodes.InvalidNamespace);

    // Test explain of findAndModify fails with an invalid collection name.
    assert.commandFailedWithCode(testDB.runCommand({explain: {findAndModify: "", update: {a: 2}}}),
                                 ErrorCodes.InvalidNamespace);
    assert.commandFailedWithCode(
        testDB.runCommand({explain: {findAndModify: "\0", update: {a: 2}}}), 17295);
    assert.commandFailedWithCode(
        testDB.runCommand({explain: {findAndModify: "a\0b", update: {a: 2}}}), 17295);

    // Test explain of delete fails with an invalid collection name.
    // TODO SERVER-24128: Currently, we massert() and print a stack trace. Instead, we should fail
    // gracefully with a user assertion.
    assert.commandFailedWithCode(
        testDB.runCommand({explain: {delete: "", deletes: [{q: {a: 1}, limit: 1}]}}), 28538);
    assert.commandFailedWithCode(
        testDB.runCommand({explain: {delete: "\0", deletes: [{q: {a: 1}, limit: 1}]}}), 17295);
    assert.commandFailedWithCode(
        testDB.runCommand({explain: {delete: "a\0b", deletes: [{q: {a: 1}, limit: 1}]}}), 17295);

    // Test explain of update fails with an invalid collection name.
    // TODO SERVER-24128: Currently, we massert() and print a stack trace. Instead, we should fail
    // gracefully with a user assertion.
    assert.commandFailedWithCode(
        testDB.runCommand({explain: {update: "", updates: [{q: {a: 1}, u: {a: 2}}]}}), 28538);
    assert.commandFailedWithCode(
        testDB.runCommand({explain: {update: "\0", updates: [{q: {a: 1}, u: {a: 2}}]}}), 17295);
    assert.commandFailedWithCode(
        testDB.runCommand({explain: {update: "a\0b", updates: [{q: {a: 1}, u: {a: 2}}]}}), 17295);

    // Test aggregate fails with an invalid collection name.
    // TODO SERVER-24128: The following two commands fail as defined in non-auth configurations,
    // but fail differently in auth configurations. We should fail identically in both
    // configurations.
    // assert.commandFailedWithCode(testDB.runCommand({aggregate: "", pipeline:[]}),
    //                              ErrorCodes.UnknownError);
    // assert.commandFailedWithCode(testDB.runCommand({aggregate: "\0", pipeline:[]}),
    //                              ErrorCodes.UnknownError);
    // TODO SERVER-24128: Make namespace parsing for group reject names with embedded null bytes.
    // TODO SERVER-24128: The following command runs successfully in non-auth configurations, but
    // fails in auth configurations. We should fail in both configurations.
    // assert.commandFailedWithCode(testDB.runCommand({aggregate: "a\0b", pipeline:[]}),
    //                              ErrorCodes.InvalidNamespace);

    // Test count fails with an invalid collection name.
    assert.commandFailedWithCode(testDB.runCommand({count: ""}), ErrorCodes.InvalidNamespace);
    assert.commandFailedWithCode(testDB.runCommand({count: "\0"}), ErrorCodes.InvalidNamespace);
    // TODO SERVER-24128: Make namespace parsing for count reject names with embedded null bytes.
    // assert.commandFailedWithCode(testDB.runCommand({count: "a\0b"}),
    // ErrorCodes.InvalidNamespace);

    // Test distinct fails with an invalid collection name.
    // TODO SERVER-24128: Currently, we massert() and print a stack trace. Instead, we should fail
    // gracefully with a user assertion.
    assert.commandFailedWithCode(testDB.runCommand({distinct: "", key: "a"}),
                                 ErrorCodes.InvalidNamespace);
    // TODO SERVER-24128: Currently, we massert() and print a stack trace. Instead, we should fail
    // gracefully with a user assertion.
    assert.commandFailedWithCode(testDB.runCommand({distinct: "\0", key: "a"}), 17295);
    // TODO SERVER-24128: Make namespace parsing for distinct reject names with embedded null bytes.
    // assert.commandFailedWithCode(testDB.runCommand({distinct: "a\0b", key: "a"}),
    // ErrorCodes.InvalidNamespace);

    // Test group fails with an invalid collection name.
    // TODO SERVER-24128: Currently, we massert() and print a stack trace. Instead, we should fail
    // gracefully with a user assertion.
    // TODO SERVER-24128: The following command fails as defined below in non-auth configurations,
    // but fails differently in auth configurations. We should fail identically in both
    // configurations.
    // assert.commandFailedWithCode(
    //     testDB.runCommand({group: {ns: "", $reduce: () => {}, initial: {}}}), 28538);
    // TODO SERVER-24128: Make namespace parsing for group reject names with embedded null bytes.
    // TODO SERVER-24128: The following two commands run successfully in non-auth configurations,
    // but fail in auth configurations. We should fail in both configurations.
    // assert.commandFailedWithCode(testDB.runCommand({group: {ns: "\0", $reduce: () => {}, initial:
    // {}}}), ErrorCodes.InvalidNamespace);
    // TODO SERVER-24128: Make namespace parsing for group reject names with embedded null bytes.
    // assert.commandFailedWithCode(testDB.runCommand({group: {ns: "a\0b", $reduce: () => {},
    // initial:
    // {}}}), ErrorCodes.InvalidNamespace);

    // Test find fails with an invalid collection name.
    assert.commandFailedWithCode(testDB.runCommand({find: ""}), ErrorCodes.InvalidNamespace);
    assert.commandFailedWithCode(testDB.runCommand({find: "\0"}), ErrorCodes.InvalidNamespace);
    // TODO SERVER-24128: Make namespace parsing for find reject names with embedded null bytes.
    // assert.commandFailedWithCode(testDB.runCommand({find: "a\0b"}), ErrorCodes.InvalidNamespace);

    // Test findAndModify fails with an invalid collection name.
    assert.commandFailedWithCode(testDB.runCommand({findAndModify: "", update: {a: 2}}),
                                 ErrorCodes.InvalidNamespace);
    assert.commandFailedWithCode(testDB.runCommand({findAndModify: "\0", update: {a: 2}}), 17295);
    assert.commandFailedWithCode(testDB.runCommand({findAndModify: "a\0b", update: {a: 2}}), 17295);

    // Test delete fails with an invalid collection name.
    assert.commandFailedWithCode(testDB.runCommand({delete: "", deletes: [{q: {a: 1}, limit: 1}]}),
                                 ErrorCodes.InvalidNamespace);
    assert.commandFailedWithCode(
        testDB.runCommand({delete: "\0", deletes: [{q: {a: 1}, limit: 1}]}), 17295);
    assert.commandFailedWithCode(
        testDB.runCommand({delete: "a\0b", deletes: [{q: {a: 1}, limit: 1}]}), 17295);

    // Test update fails with an invalid collection name.
    assert.commandFailedWithCode(testDB.runCommand({update: "", updates: [{q: {a: 1}, u: {a: 2}}]}),
                                 ErrorCodes.InvalidNamespace);
    assert.commandFailedWithCode(
        testDB.runCommand({update: "\0", updates: [{q: {a: 1}, u: {a: 2}}]}), 17295);
    assert.commandFailedWithCode(
        testDB.runCommand({update: "a\0b", updates: [{q: {a: 1}, u: {a: 2}}]}), 17295);

    // Stop the sharding test.
    st.stop();
})();
