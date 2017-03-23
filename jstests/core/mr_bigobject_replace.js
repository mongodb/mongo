/**
 * Test that the server returns an error response for map-reduce operations that attempt to insert a
 * document larger than 16MB as a result of the reduce() or finalize() functions and using the
 * "replace" action for the out collection.
 */
(function() {
    function mapper() {
        // Emit multiple values to ensure that the reducer gets called.
        emit(this._id, 1);
        emit(this._id, 1);
    }

    function createBigDocument() {
        // Returns a document of the form { _id: ObjectId(...), value: '...' } with the specified
        // 'targetSize' in bytes.
        function makeDocWithSize(targetSize) {
            var doc = {_id: new ObjectId(), value: ''};

            var size = Object.bsonsize(doc);
            assert.gte(targetSize, size);

            // Set 'value' as a string with enough characters to make the whole document 'size'
            // bytes long.
            doc.value = new Array(targetSize - size + 1).join('x');
            assert.eq(targetSize, Object.bsonsize(doc));

            return doc;
        }

        var maxDocSize = 16 * 1024 * 1024;
        return makeDocWithSize(maxDocSize + 1).value;
    }

    function runTest(testOptions) {
        db.input.drop();
        db.mr_bigobject_replace.drop();

        // Insert a document so the mapper gets run.
        assert.writeOK(db.input.insert({}));

        var res = db.runCommand(Object.extend({
            mapReduce: "input",
            map: mapper,
            out: {replace: "mr_bigobject_replace"},
        },
                                              testOptions));

        assert.commandFailed(res, "creating a document larger than 16MB didn't fail");
        assert.lte(0,
                   res.errmsg.indexOf("object to insert too large"),
                   "map-reduce command failed for a reason other than inserting a large document");
    }

    runTest({reduce: createBigDocument});
    runTest({
        reduce: function() {
            return 1;
        },
        finalize: createBigDocument
    });
})();
