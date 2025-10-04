// @tags: [
//   requires_getmore,
// ]

// Sanity check for the showRecordId option.

let t = db.show_record_id;
t.drop();

function checkResults(arr) {
    for (let i in arr) {
        let a = arr[i];
        assert(a["$recordId"]);
    }
}

// Check query.
t.save({});
checkResults(t.find().showRecordId().toArray());

// Check query and get more.
t.save({});
t.save({});
checkResults(t.find().batchSize(2).showRecordId().toArray());

// Check with a covered index.
t.createIndex({a: 1});
checkResults(t.find({}, {_id: 0, a: 1}).hint({a: 1}).showRecordId().toArray());
checkResults(t.find({}, {_id: 0, a: 1}).hint({a: 1}).showRecordId().toArray());

// Check with an idhack query.
t.drop();
t.save({_id: 0, a: 1});
checkResults(t.find({_id: 0}).showRecordId().toArray());
