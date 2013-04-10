// Test handling of messages up to and over isMaster().maxMessageSizeBytes

function go() { // using a function to ensure that all resources can be freed after test
    if (db.serverStatus().mem.bits == 32) {
        print("skipping max_message_size.js on 32bit system");
        return;
    }

    var t = db.max_message_size;

    var maxMessageSize = db.isMaster().maxMessageSizeBytes;
    var maxBsonSize = db.isMaster().maxBsonObjectSize;

    function makeObj(str) {
        return {_id: ObjectId(), s: str};
    }

    var bsonOverhead = Object.bsonsize(makeObj(''));

    var bigStr = 'x';
    while (bigStr.length < maxBsonSize) {
        bigStr += bigStr;
    }

    function insertWithBytes(bytes) {
        var toGo = bytes;
        toGo -= 16; // Message Header
        toGo -= 4; // Flags
        toGo -= t.getFullName().length + 1; // namespace with NUL

        var batch = [];
        while (toGo > 0) {
            var objBytes = Math.min(toGo, maxBsonSize);
            var filler = bigStr.substr(0, objBytes - bsonOverhead);
            var obj = makeObj(filler);
            assert.eq(Object.bsonsize(obj), objBytes);
            batch.push(obj);
            toGo -= objBytes;
        }
        assert.eq(toGo, 0);

        t.insert(batch);

        return batch.length;
    }

    function works(bytes) {
        t.drop();
        var numInserted = insertWithBytes(bytes);
        assert.isnull(db.getLastError());
        assert.eq(t.count(), numInserted); 
    }

    function fails(bytes) {
        t.drop();

        try {
            var numInserted = insertWithBytes(bytes);
            var error = db.getLastErrorObj();
        } catch (e) {
            // A string is thrown rather than an object
            if (! (/^socket error/.test(e) || /socket exception/.test(e)))
                throw e;

            sleep(3000); // shell won't reconnect within 2 second window

            assert.eq(t.count(), 0); 
            return; // successfully killed connection and reconnected
        }

        // Note to future maintainers: This test will need to be changed if we
        // modify the server's behavior to skip oversized messages and report
        // them in getLastError. The output from this should be helpful in
        // detecting this case.
        printjson({numInserted: numInserted, error: error});
        assert(false, "Connection not reset");
    }

    works(maxMessageSize - 1024*1024);
    works(maxMessageSize - 1);
    works(maxMessageSize);

    fails(maxMessageSize + 1);
    works(maxMessageSize); // make sure we still work after failure
    fails(maxMessageSize + 1024*1024);
    works(maxMessageSize);
}
go();
