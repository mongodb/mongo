(function() {
    'use strict';

    // PV0
    assert.eq(-1, rs.compareOpTimes(Timestamp(1, 0), Timestamp(2, 0)));
    assert.eq(-1, rs.compareOpTimes(Timestamp(1, 0), Timestamp(1, 1)));

    assert.eq(0, rs.compareOpTimes(Timestamp(2, 0), Timestamp(2, 0)));

    assert.eq(1, rs.compareOpTimes(Timestamp(2, 0), Timestamp(1, 1)));
    assert.eq(1, rs.compareOpTimes(Timestamp(1, 2), Timestamp(1, 1)));

    // lhs PV0 rhs PV1
    assert.throws(function() {
        rs.compareOpTimes(Timestamp(1, 0), {ts: Timestamp(2, 0), t: 2});
    });

    // lhs PV1 rhs PV0
    assert.throws(function() {
        rs.compareOpTimes({ts: Timestamp(1, 0), t: 2}, Timestamp(2, 0));
    });

    // PV1
    assert.eq(-1, rs.compareOpTimes({ts: Timestamp(2, 2), t: 2}, {ts: Timestamp(3, 1), t: 2}));
    assert.eq(-1, rs.compareOpTimes({ts: Timestamp(2, 2), t: 2}, {ts: Timestamp(2, 4), t: 2}));
    assert.eq(-1, rs.compareOpTimes({ts: Timestamp(3, 0), t: 2}, {ts: Timestamp(2, 0), t: 3}));

    assert.eq(0, rs.compareOpTimes({ts: Timestamp(3, 0), t: 2}, {ts: Timestamp(3, 0), t: 2}));

    assert.eq(1, rs.compareOpTimes({ts: Timestamp(3, 1), t: 2}, {ts: Timestamp(2, 2), t: 2}));
    assert.eq(1, rs.compareOpTimes({ts: Timestamp(2, 4), t: 2}, {ts: Timestamp(2, 2), t: 2}));
    assert.eq(1, rs.compareOpTimes({ts: Timestamp(2, 0), t: 3}, {ts: Timestamp(3, 0), t: 2}));

})();
