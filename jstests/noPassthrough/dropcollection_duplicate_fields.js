/*
 * SERVER-35172: Test that dropCollection does not return a message containing multiple "ns" fields
 * @tags: [requires_wiredtiger]
 */

(function() {
    "use strict";
    var conn = MongoRunner.runMongod();
    var db = conn.getDB('test');

    let coll = db.dropcollection_duplicate_fields;
    // Repeat 100 times for the sake of probabilities
    for (let i = 0; i < 100; i++) {
        coll.drop();
        coll.insert({x: 1});

        assert.commandWorked(db.adminCommand(
            {configureFailPoint: 'WTWriteConflictException', mode: {activationProbability: 0.1}}));

        // will blow up if res is not valid
        let res = db.runCommand({drop: 'dropcollection_duplicate_fields'});

        assert.commandWorked(
            db.adminCommand({configureFailPoint: 'WTWriteConflictException', mode: "off"}));
    }

    MongoRunner.stopMongod(conn);

})();
