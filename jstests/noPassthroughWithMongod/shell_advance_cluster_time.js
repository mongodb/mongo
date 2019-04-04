/**
 * Santity check getClusterTime and advanceClusterTime.
 */

(function() {
    assert.throws(function() {
        db.getMerizo().advanceClusterTime();
    });

    assert.throws(function() {
        db.getMerizo().advanceClusterTime(123);
    });

    assert.throws(function() {
        db.getMerizo().advanceClusterTime('abc');
    });

    db.getMerizo().advanceClusterTime({'clusterTime': 123});

    assert.eq({'clusterTime': 123}, db.getMerizo().getClusterTime());

    db.getMerizo().advanceClusterTime({'clusterTime': 100});

    assert.eq({'clusterTime': 123}, db.getMerizo().getClusterTime());

    db.getMerizo().advanceClusterTime({'clusterTime': 456});

    assert.eq({'clusterTime': 456}, db.getMerizo().getClusterTime());
})();
