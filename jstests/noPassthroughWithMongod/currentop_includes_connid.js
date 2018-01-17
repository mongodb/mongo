(function() {
    "use strict";

    let res = assert.commandWorked(db.runCommand({whatsmyuri: 1}));
    const myUri = res.you;

    res = assert.commandWorked(db.adminCommand({currentOp: 1, client: myUri}));
    const threadName = res.inprog[0].desc;
    const connectionId = res.inprog[0].connectionId;

    assert.eq("conn" + connectionId, threadName, tojson(res));
})();
