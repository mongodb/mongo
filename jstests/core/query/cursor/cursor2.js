/**
 * Test to see if the count returned from the cursor is the number of objects that would be
 * returned
 *
 * BUG 884
 *
 * @tags: [requires_fastcount, requires_getmore]
 */
function testCursorCountVsArrLen(dbConn) {
    let coll = dbConn.ed_db_cursor2_ccvsal;

    coll.drop();

    coll.save({a: 1, b: 1});
    coll.save({a: 2, b: 1});
    coll.save({a: 3});

    let fromCount = coll.find({}, {b: 1}).count();
    let fromArrLen = coll.find({}, {b: 1}).toArray().length;

    assert(fromCount == fromArrLen, "count from cursor [" + fromCount + "] !=  count from arrlen [" + fromArrLen + "]");
}

testCursorCountVsArrLen(db);
