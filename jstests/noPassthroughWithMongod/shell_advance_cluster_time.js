/**
 * Santity check getClusterTime and advanceClusterTime.
 */

(function() {
    assert.throws(function() {
        db.getMongo().advanceClusterTime();
    });

    assert.throws(function() {
        db.getMongo().advanceClusterTime(123);
    });

    assert.throws(function() {
        db.getMongo().advanceClusterTime('abc');
    });

    db.getMongo().advanceClusterTime({'clusterTime': 123});

    assert.eq({'clusterTime': 123}, db.getMongo().getClusterTime());

    db.getMongo().advanceClusterTime({'clusterTime': 100});

    assert.eq({'clusterTime': 123}, db.getMongo().getClusterTime());

    db.getMongo().advanceClusterTime({'clusterTime': 456});

    assert.eq({'clusterTime': 456}, db.getMongo().getClusterTime());
})();
