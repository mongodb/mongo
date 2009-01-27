
/**
 *  test to see if the count returned from the cursor is the number of objects that would be returned
 *
 *  BUG 884
 */
function testCursorCountVsArrLen(dbConn) {

    // uncomment when BUG 884 is fixed
    print("ed/db/cursor2.js:testCursorCountVsArrLen() - not run - BUG 884");
    return;

    var coll = dbConn.ed_db_cursor2_ccvsal;

    coll.drop();

    coll.save({ a: 1, b : 1});
    coll.save({ a: 2, b : 1});
    coll.save({ a: 3});

    var fromCount = coll.find({}, {b:1}).count();
    var fromArrLen = coll.find({}, {b:1}).toArray().length;

    assert(fromCount == fromArrLen, "count from cursor [" + fromCount + "] !=  count from arrlen [" + fromArrLen + "]");
}


db = connect("test")
testCursorCountVsArrLen(db);
