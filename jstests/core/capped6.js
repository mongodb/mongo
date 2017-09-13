// Test NamespaceDetails::cappedTruncateAfter via "captrunc" command
(function() {
    var coll = db.capped6;

    Random.setRandomSeed();
    var maxDocuments = Random.randInt(400) + 100;

    /**
     * Check that documents in the collection are in order according to the value
     * of a, which corresponds to the insert order.  This is a check that the oldest
     * document(s) is/are deleted when space is needed for the newest document.  The
     * check is performed in both forward and reverse directions.
     */
    function checkOrder(i, valueArray) {
        res = coll.find().sort({$natural: -1});
        assert(res.hasNext(), "A");
        var j = i;
        while (res.hasNext()) {
            assert.eq(valueArray[j--].a, res.next().a, "B");
        }

        res = coll.find().sort({$natural: 1});
        assert(res.hasNext(), "C");
        while (res.hasNext()) {
            assert.eq(valueArray[++j].a, res.next().a, "D");
        }
        assert.eq(j, i, "E");
    }

    /*
     * Prepare the values to insert and create the capped collection.
     */
    function prepareCollection(shouldReverse) {
        coll.drop();
        db._dbCommand(
            {create: "capped6", capped: true, size: 1000, $nExtents: 11, autoIndexId: false});
        var valueArray = new Array(maxDocuments);
        var c = "";
        for (i = 0; i < maxDocuments; ++i, c += "-") {
            // The a values are strings of increasing length.
            valueArray[i] = {a: c};
        }
        if (shouldReverse) {
            valueArray.reverse();
        }
        return valueArray;
    }

    /**
     * 1. When this function is called the first time, insert new documents until 'maxDocuments'
     *    number of documents have been inserted. Note that the collection may not have
     *    'maxDocuments' number of documents since it is a capped collection.
     * 2. Remove all but one documents via one or more "captrunc" requests.
     * 3. For each subsequent call to this function, keep track of the removed documents using
     *    'valueArrayIndexes' and re-insert the removed documents each time this function is
     *    called.
     */
    function runCapTrunc(valueArray, valueArrayCurIndex, n, inc) {
        // If n <= 0, no documents are removed by captrunc.
        assert.gt(n, 0);
        assert.gte(valueArray.length, maxDocuments);
        for (var i = valueArrayCurIndex; i < maxDocuments; ++i) {
            assert.writeOK(coll.insert(valueArray[i]));
        }
        count = coll.count();

        // The index corresponding to the last document in the collection.
        valueArrayCurIndex = maxDocuments - 1;

        // Number of times to call "captrunc" so that (count - 1) documents are removed
        // and at least 1 document is left in the array.
        var iterations = Math.floor((count - 1) / (n + inc));

        for (i = 0; i < iterations; ++i) {
            assert.commandWorked(db.runCommand({captrunc: "capped6", n: n, inc: inc}));
            count -= (n + inc);
            valueArrayCurIndex -= (n + inc);
            checkOrder(valueArrayCurIndex, valueArray);
        }
        // We return the index of the next document that should be inserted into the capped
        // collection, which would be the document after valueArrayCurIndex.
        return valueArrayCurIndex + 1;
    }

    function doTest(shouldReverse) {
        var valueArray = prepareCollection(shouldReverse);
        var valueArrayIndex = 0;
        valueArrayIndex = runCapTrunc(valueArray, valueArrayIndex, 1, false);
        valueArrayIndex = runCapTrunc(valueArray, valueArrayIndex, 1, true);
        valueArrayIndex = runCapTrunc(valueArray, valueArrayIndex, 16, true);
        valueArrayIndex = runCapTrunc(valueArray, valueArrayIndex, 16, false);
        valueArrayIndex = runCapTrunc(valueArray, valueArrayIndex, maxDocuments - 2, true);
        valueArrayIndex = runCapTrunc(valueArray, valueArrayIndex, maxDocuments - 2, false);
    }

    // Repeatedly add up to 'maxDocuments' documents and then truncate the newest
    // documents.  Newer documents take up more space than older documents.
    doTest(false);

    // Same test as above, but now the newer documents take less space than the
    // older documents instead of more.
    doTest(true);
})();
