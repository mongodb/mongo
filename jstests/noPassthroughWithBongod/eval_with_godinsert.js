// Test for SERVER-26911. Executing a godinsert and a findAndModify in the same eval should not
// crash the server.
(function() {
    "use strict";

    db.jstests_server26911.drop();

    assert.writeOK(db.jstests_server26911.insert({_id: 0, a: 0}));

    const godinsertCmd =
        "assert.commandWorked(db.runCommand({godinsert: 'jstests_server26911', obj: {_id: 1}}));";
    const findAndModifyCmd = "db.jstests_server26911.findOneAndReplace({a: 0}, {a: 1});";

    assert.eq({_id: 0, a: 0}, db.eval(godinsertCmd + findAndModifyCmd));
}());