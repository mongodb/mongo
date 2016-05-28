// SERVER-4516 and SERVER-6913: test that update and findAndModify tolerate
// an _id in the update document, as long as the _id will not be modified

var t = db.jstests_server4516;
var startingDoc = {_id: 1, a: 1};

function prepare() {
    t.drop();
    t.save(startingDoc);
}

function update_succeeds(updateDoc, qid, resultDoc) {
    prepare();
    t.update({_id: qid}, updateDoc, true);
    assert.eq(t.findOne({_id: qid}), resultDoc);

    prepare();
    t.findAndModify({query: {_id: qid}, update: updateDoc, upsert: true});
    assert.eq(t.findOne({_id: qid}), resultDoc);
}

update_succeeds({_id: 1, a: 2}, 1, {_id: 1, a: 2});
update_succeeds({$set: {_id: 1}}, 1, {_id: 1, a: 1});
update_succeeds({_id: 1, b: "a"}, 1, {_id: 1, b: "a"});
update_succeeds({_id: 2, a: 3}, 2, {_id: 2, a: 3});

function update_fails(updateDoc, qid) {
    prepare();
    var res = t.update({_id: qid}, updateDoc, true);
    assert.writeError(res);
    assert.eq(t.count(), 1);
    assert.eq(t.findOne(), startingDoc);

    prepare();
    assert.throws(function() {
        t.findAndModify({query: {_id: qid}, update: updateDoc, upsert: true});
    });
    assert.eq(t.count(), 1);
    assert.eq(t.findOne(), startingDoc);
}

update_fails({$set: {_id: 2}}, 1);
update_fails({_id: 2, a: 3}, 1);
update_fails({_id: 2, a: 3}, 3);
