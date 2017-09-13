(function() {

    "use strict";
    var t = db.jstests_server23626;

    t.mycoll.drop();
    assert.writeOK(t.mycoll.insert({_id: 0, a: Date.prototype}));
    assert.eq(1, t.mycoll.find({a: {$type: 'date'}}).itcount());

    t.mycoll.drop();
    assert.writeOK(t.mycoll.insert({_id: 0, a: Function.prototype}));
    assert.eq(1, t.mycoll.find({a: {$type: 'javascript'}}).itcount());

    t.mycoll.drop();
    assert.writeOK(t.mycoll.insert({_id: 0, a: RegExp.prototype}));
    assert.eq(1, t.mycoll.find({a: {$type: 'regex'}}).itcount());

}());