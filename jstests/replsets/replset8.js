
// test for SERVER-6303 - if documents move backward during an initial sync.

(function() {
    "use strict";
    var rt = new ReplSetTest({name: "replset8", nodes: 1});

    var nodes = rt.startSet();
    rt.initiate();
    var master = rt.getPrimary();
    var bigstring = "a";
    var md = master.getDB('d');
    var mdc = md['c'];

    // prep the data

    // idea: create x documents of increasing size, then create x documents of size n.
    //       delete first x documents.  start initial sync (cloner).  update all remaining
    //       documents to be increasing size.
    //       this should result in the updates moving the docs backwards.

    var doccount = 5000;
    // Avoid empty extent issues
    mdc.insert({_id: -1, x: "dummy"});

    jsTestLog('inserting ' + doccount + ' bigstrings');
    var bulk = mdc.initializeUnorderedBulkOp();
    for (var i = 0; i < doccount; ++i) {
        bulk.insert({_id: i, x: bigstring});
        bigstring += "a";
    }
    var result = assert.writeOK(bulk.execute());
    jsTestLog('insert 0-' + (doccount - 1) + ' result: ' + tojson(result));
    assert.eq(doccount, result.nInserted);
    assert.eq(doccount + 1, mdc.find().itcount());

    jsTestLog('inserting ' + (doccount * 2) + ' documents - {_id: 0, x: 0} ... {_id: ' +
              (doccount * 2 - 1) + ', x: ' + (doccount * 2 - 1) + '}');
    bulk = mdc.initializeUnorderedBulkOp();
    for (i = doccount; i < doccount * 2; ++i) {
        bulk.insert({_id: i, x: i});
    }
    result = assert.writeOK(bulk.execute());
    jsTestLog('insert ' + doccount + '-' + (doccount * 2 - 1) + ' result: ' + tojson(result));
    assert.eq(doccount, result.nInserted);
    assert.eq(doccount * 2 + 1, mdc.find().itcount());

    jsTestLog('deleting ' + doccount + ' bigstrings');
    bulk = mdc.initializeUnorderedBulkOp();
    for (i = 0; i < doccount; ++i) {
        bulk.find({_id: i}).remove();
    }
    result = assert.writeOK(bulk.execute());
    jsTestLog('delete 0-' + (doccount - 1) + ' result: ' + tojson(result));
    assert.eq(doccount, result.nRemoved);
    assert.eq(doccount + 1, mdc.find().itcount());

    // add a secondary
    var slave = rt.add();
    rt.reInitiate();
    jsTestLog('reinitiation complete after adding new node to replicaset');
    rt.awaitSecondaryNodes();
    jsTestLog("updating documents backwards");
    // Move all documents to the beginning by growing them to sizes that should
    // fit the holes we made in phase 1
    bulk = mdc.initializeUnorderedBulkOp();
    for (i = doccount * 2; i > doccount; --i) {
        bulk.find({_id: i}).update({$set: {x: bigstring}});
        bigstring = bigstring.slice(0, -1);  // remove last char
    }
    result = assert.writeOK(bulk.execute({w: rt.nodes.length}));
    jsTestLog('update ' + (doccount + 1) + '-' + (doccount * 2 - 1) + ' result: ' + tojson(result));
    assert.eq(doccount - 1, result.nMatched);
    assert.eq(doccount - 1, result.nModified);

    assert.eq(doccount + 1,
              mdc.find().itcount(),
              'incorrect collection size on primary (fast count: ' + mdc.count() + ')');
    assert.eq(doccount + 1,
              slave.getDB('d')['c'].find().itcount(),
              'incorrect collection size on secondary  (fast count: ' +
                  slave.getDB('d')['c'].count() + ')');

    jsTestLog("finished");
})();
