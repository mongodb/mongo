/**
 * Santity check getClusterTime and setClusterTime.
 */

(function() {
    assert.throws(function() {
        db.getMongo().setClusterTime();
    });

    assert.throws(function() {
        db.getMongo().setClusterTime(123);
    });

    assert.throws(function() {
        db.getMongo().setClusterTime('abc');
    });

    db.getMongo().setClusterTime({'clusterTime': 123});

    assert.eq({'clusterTime': 123}, db.getMongo().getClusterTime());

    db.getMongo().setClusterTime({'clusterTime': 100});

    assert.eq({'clusterTime': 123}, db.getMongo().getClusterTime());

    db.getMongo().setClusterTime({'clusterTime': 456});

    assert.eq({'clusterTime': 456}, db.getMongo().getClusterTime());
})();
