// SERVER-23326: Make applyOps atomic for CRUD operations
(function() {
    'use strict';

    var t = db.applyOps;
    t.drop();
    assert.writeOK(t.insert({_id: 1}));

    // Operations including commands should not be atomic, so the insert will succeed.
    assert.commandFailed(db.adminCommand({
        applyOps: [
            {op: 'i', ns: t.getFullName(), o: {_id: ObjectId(), x: 1}},
            {op: 'c', ns: "invalid", o: {create: "t"}},
        ]
    }));
    assert.eq(t.count({x: 1}), 1);

    // Operations only including CRUD commands should be atomic, so the next insert will fail.
    var tooLong = Array(2000).join("hello");
    assert.commandFailed(db.adminCommand({
        applyOps: [
            {op: 'i', ns: t.getFullName(), o: {_id: ObjectId(), x: 1}},
            {op: 'i', ns: t.getFullName(), o: {_id: tooLong, x: 1}},
        ]
    }));
    assert.eq(t.count({x: 1}), 1);
})();
