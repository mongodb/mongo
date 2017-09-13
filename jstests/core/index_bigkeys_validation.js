// Tests that index validation succeeds for long keys when failIndexKeyTooLong is set to false.
// See: SERVER-22234
'use strict';

(function() {
    var coll = db.longindex;
    coll.drop();

    function checkCollectionIsValid({valid, full}) {
        var res = coll.validate(full);
        assert.commandWorked(res);
        assert.eq(res.valid, valid, tojson(res));
        // Verify that the top level response object is consistent with the index-specific one.
        if (full) {
            assert.eq(
                res.valid, res.indexDetails[coll.getFullName() + '.$_id_'].valid, tojson(res));
        }
    }

    // Keys >= 1024 bytes cannot be indexed. A string represented as a BSON object has a 12 byte
    // overhead. So a 1012 byte string corresponds to a 1024 byte key.
    // The 12 byte overhead consists of `int32, \x02, \x00, int32` before the raw string and
    // `\00, \00` after it. For details, see: http://bsonspec.org/spec.html
    var longVal = 'x'.repeat(1012);

    // Verify that validation succeeds when the key is < 1024 bytes.
    var boundaryVal = 'x'.repeat(1011);
    assert.writeOK(coll.insert({_id: boundaryVal}));
    checkCollectionIsValid({valid: true, full: false});
    checkCollectionIsValid({valid: true, full: true});

    assert.commandWorked(db.adminCommand({setParameter: 1, failIndexKeyTooLong: false}));

    assert.writeOK(coll.insert({_id: longVal}));
    // Verify that validation succeeds when the failIndexKeyTooLong parameter is set to false,
    // even when there are fewer index keys than documents.
    checkCollectionIsValid({valid: true, full: false});
    checkCollectionIsValid({valid: true, full: true});

    // Change failIndexKeyTooLong back to the default value.
    assert.commandWorked(db.adminCommand({setParameter: 1, failIndexKeyTooLong: true}));

    // Verify that a non-full validation succeeds when the failIndexKeyTooLong parameter is
    // reverted to its old value and there are mismatched index keys and documents.
    // Since during the collection scan, we see the un-indexed long keys in the
    // documents keys.
    checkCollectionIsValid({valid: true, full: false});

    // Verify that a full validation still succeeds.
    checkCollectionIsValid({valid: true, full: true});

    // Explicitly drop the collection to avoid failures in post-test hooks that run dbHash and
    // validate commands.
    coll.drop();
})();
