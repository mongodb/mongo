// Test for the bug found in SERVER-39612, where the server could behave poorly on invalid input
// from a $geoNear.
(function() {
    const coll = db.geonear_validation;

    coll.drop();
    coll.insert({});
    coll.createIndex({"a.b.c": "2dsphere"});

    function runGeoNearWithKey(key) {
        return coll.aggregate([{
            $geoNear: {
                key: key,
                spherical: true,
                distanceField: 'distance',
                near: {type: 'Point', coordinates: [0, 0]}
            }
        }]);
    }

    function runGeoNearCmdWithKey(key) {
        const geoCmd = {
            geoNear: coll.getName(),
            near: {type: 'Point', coordinates: [0, 0]},
            spherical: true,
            key: key
        };
        return db.runCommand(geoCmd);
    }

    // Try a valid value for 'key'.
    (function() {
        const key = "a.b.c";
        const cursor = runGeoNearWithKey(key);
        assert.eq(cursor.itcount(), 0);

        const cmdCursor = assert.commandWorked(runGeoNearCmdWithKey(key));
        assert.eq(cursor.itcount(), 0);
    })();

    (function() {
        // 'key' cannot start with a "$" since it would be an invalid field name.
        const key = "$hello";
        const code = 16410;
        let err = assert.throws(() => runGeoNearWithKey(key));
        assert.eq(err.code, code);

        assert.commandFailedWithCode(runGeoNearCmdWithKey(key), code);
    })();

    (function() {
        // 'key' cannot include null bytes since it would make it an invalid field name.
        const key = "A\0B";
        const code = 16411;
        const err = assert.throws(() => runGeoNearWithKey(key));
        assert.eq(err.code, code);

        assert.commandFailedWithCode(runGeoNearCmdWithKey(key), code);
    })();
})();
