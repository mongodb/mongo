// SERVER-27996/SERVER-28022 Missing invalidation for system.namespaces writes
(function() {
    'use strict';
    let dbInvalidName = 'system_namespaces_invalidations';
    let dbInvalid = db.getSiblingDB(dbInvalidName);
    let num_collections = 3;
    let DROP = 1;
    let RENAME = 2;
    let MOVE = 3;
    function testNamespaceInvalidation(namespaceAction, batchSize) {
        dbInvalid.dropDatabase();

        // Create enough collections to necessitate multiple cursor batches.
        for (let i = 0; i < num_collections; i++) {
            assert.commandWorked(dbInvalid.createCollection('coll' + i.toString()));
        }

        // Get the first two namespaces. Use find on 'system.namespaces' on MMAPv1, listCollections
        // otherwise.
        let cmd = dbInvalid.system.indexes.count() ? {find: 'system.namespaces'}
                                                   : {listCollections: dbInvalidName};
        Object.extend(cmd, {batchSize: batchSize});
        let res = dbInvalid.runCommand(cmd);
        assert.commandWorked(res, 'could not run ' + tojson(cmd));
        printjson(res);

        // Ensure the cursor has data, invalidate the namespace, and exhaust the cursor.
        let cursor = new DBCommandCursor(dbInvalid.getMongo(), res);
        let errMsg =
            'expected more data from command ' + tojson(cmd) + ', with result ' + tojson(res);
        assert(cursor.hasNext(), errMsg);
        if (namespaceAction == RENAME) {
            // Rename the collection to something that does not fit in the previously allocated
            // memory for the record.
            assert.commandWorked(
                dbInvalid['coll1'].renameCollection('coll1' +
                                                    'lkdsahflaksjdhfsdkljhfskladhfkahfsakfla' +
                                                    'skfjhaslfaslfkhasklfjhsakljhdsjksahkldjslh'));
        } else if (namespaceAction == DROP) {
            assert(dbInvalid['coll1'].drop());
        } else if (namespaceAction == MOVE) {
            let modCmd = {
                collMod: 'coll1',
                validator: {
                    $or: [
                        {phone: {$type: "string"}},
                        {email: {$regex: /@mongodb\.com$/}},
                        {status: {$in: ["Unknown", "Incomplete"]}},
                        {address: {$type: "string"}},
                        {ssn: {$type: "string"}},
                        {favoriteBook: {$type: "string"}},
                        {favoriteColor: {$type: "string"}},
                        {favoriteBeverage: {$type: "string"}},
                        {favoriteDay: {$type: "string"}},
                        {favoriteFood: {$type: "string"}},
                        {favoriteSport: {$type: "string"}},
                        {favoriteMovie: {$type: "string"}},
                        {favoriteShow: {$type: "string"}}
                    ]
                }
            };
            assert.commandWorked(dbInvalid.runCommand(modCmd));
        }
        assert.gt(cursor.itcount(), 0, errMsg);
    }
    // Test that we invalidate the old namespace record ID when we remove, rename, or move a
    // namespace record.
    for (let j = 2; j < 7; j++) {
        testNamespaceInvalidation(DROP, j);
        testNamespaceInvalidation(RENAME, j);
        testNamespaceInvalidation(MOVE, j);
    }
}());
