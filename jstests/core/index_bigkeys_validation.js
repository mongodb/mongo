// Tests that index validation succeeds for long keys when failIndexKeyTooLong is set to false.
// See: SERVER-22234
'use strict';

(function() {
    var coll = db.longindex;
    coll.drop();

    var longVal = new Array(1025).join('x'); // Keys >= 1024 bytes cannot be indexed.

    assert.commandWorked(db.adminCommand({setParameter: 1, failIndexKeyTooLong: false}));

    assert.writeOK(coll.insert({_id: longVal}));
    // Verify that validation succeeds when the failIndexKeyTooLong parameter is set to false,
    // even when there are fewer index keys than documents.
    var res = coll.validate({full: true, scandata: true});
    assert.commandWorked(res);
    assert(res.valid, tojson(res));

    // Change failIndexKeyTooLong back to the default value.
    assert.commandWorked(db.adminCommand({setParameter: 1, failIndexKeyTooLong: true}));

    // Verify that validation fails when the failIndexKeyTooLong parameter is
    // reverted to its old value and there are mismatched index keys and documents.
    res = coll.validate({full: true, scandata: true});
    assert.commandWorked(res);
    assert.eq(res.valid, false, tojson(res));
})();
