(function() {
    // SERVER-28285 When renameCollection drops the target collection, it should just generate
    // a single oplog entry, so we cannot end up in a state where the drop has succeeded, but
    // the rename didn't.
    let rs = new ReplSetTest({nodes: 1});
    rs.startSet();
    rs.initiate();

    let prim = rs.getPrimary();
    let first = prim.getDB("first");
    let second = prim.getDB("second");
    let local = prim.getDB("local");

    // Test both for rename within a database as across databases.
    const tests = [
        {
          source: first.x,
          target: first.y,
          expectedOplogEntries: 1,
        },
        {
          source: first.x,
          target: second.x,
          expectedOplogEntries: 4,
        }
    ];
    tests.forEach((test) => {
        test.source.drop();
        assert.writeOK(test.source.insert({}));
        assert.writeOK(test.target.insert({}));

        let ts = local.oplog.rs.find().sort({$natural: -1}).limit(1).next().ts;
        let cmd = {
            renameCollection: test.source.toString(),
            to: test.target.toString(),
            dropTarget: true
        };
        assert.commandWorked(local.adminCommand(cmd), tojson(cmd));
        ops = local.oplog.rs.find({ts: {$gt: ts}}).sort({$natural: 1}).toArray();
        assert.eq(ops.length,
                  test.expectedOplogEntries,
                  "renameCollection was supposed to only generate " + test.expectedOplogEntries +
                      " oplog entries: " + tojson(ops));
    });
    rs.stopSet();
})();
