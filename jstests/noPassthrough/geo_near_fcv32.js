/**
 * Confirms that $geoNear aggregation and geoNear command succeed when FCV is 3.2.
 */
(function() {
    "use strict";

    const conn = MongoRunner.runMongod({});
    assert.neq(null, conn, "mongod was unable to start up");

    const testDB = conn.getDB("geo_near_fcv32");
    testDB.test32.drop();
    assert.commandWorked(testDB.adminCommand({setFeatureCompatibilityVersion: "3.2"}));

    // Create 2dsphere index.
    assert.commandWorked(testDB.test32.createIndex({loc: "2dsphere"}));

    // Assert that $geoNear aggregate command does not fail due to collation errors when FCV is 3.2.
    assert.eq(0,
              testDB.test32
                  .aggregate([{
                      $geoNear: {
                          near: {type: "Point", coordinates: [1.23, 1.23]},
                          distanceField: "distance",
                          spherical: true
                      }
                  }])
                  .itcount());

    // Assert that specifying the simple collation in $geoNear aggregate fails with FCV 3.2.
    assert.throws(function() {
        testDB.test32.aggregate([{
            $geoNear: {
                near: {type: "Point", coordinates: [1.23, 1.23]},
                distanceField: "distance",
                spherical: true,
                collation: {locale: "simple"},
            }
        }]);
    });

    // Assert that specifying the simple collation in geoNear command fails with FCV 3.2.
    assert.commandFailed(testDB.runCommand({
        geoNear: "test32",
        near: {type: "Point", coordinates: [1.23, 1.23]},
        spherical: true,
        collation: {locale: "simple"}
    }));

    assert.commandWorked(testDB.adminCommand({setFeatureCompatibilityVersion: "3.4"}));

    // Create collection with case sensitive collation.
    assert.commandWorked(
        testDB.createCollection("test34", {collation: {locale: "en_US", strength: 2}}));
    assert.commandWorked(testDB.test34.createIndex({loc: "2dsphere"}));
    assert.writeOK(testDB.test34.insert({loc: [1.23, 1.23], str: "A"}));

    // Assert that after upgrading FCV to 3.4 $geoNear aggregate inherits the collection's default
    // collation.
    assert.eq(1,
              testDB.test34
                  .aggregate([{
                      $geoNear: {
                          near: {type: "Point", coordinates: [1.23, 1.23]},
                          distanceField: "distance",
                          spherical: true,
                          query: {str: "a"},
                      }
                  }])
                  .itcount());

    // Assert that the $geoNear aggregate accepts a specific collation and overrides the default
    // collation in FCV 3.4.
    assert.eq(1,
              testDB.test34
                  .aggregate([{
                                $geoNear: {
                                    near: {type: "Point", coordinates: [1.23, 1.23]},
                                    distanceField: "distance",
                                    spherical: true,
                                    query: {str: "à"},
                                }
                             }],
                             {collation: {locale: "en_US", strength: 1}})
                  .itcount());
})();
