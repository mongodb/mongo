(function() {
'use strict';

// PV1
assert.eq(-1, rs.compareOpTimes({ts: Timestamp(2, 2), t: 2}, {ts: Timestamp(3, 1), t: 2}));
assert.eq(-1, rs.compareOpTimes({ts: Timestamp(2, 2), t: 2}, {ts: Timestamp(2, 4), t: 2}));
assert.eq(-1, rs.compareOpTimes({ts: Timestamp(3, 0), t: 2}, {ts: Timestamp(2, 0), t: 3}));

assert.eq(0, rs.compareOpTimes({ts: Timestamp(3, 0), t: 2}, {ts: Timestamp(3, 0), t: 2}));

assert.eq(1, rs.compareOpTimes({ts: Timestamp(3, 1), t: 2}, {ts: Timestamp(2, 2), t: 2}));
assert.eq(1, rs.compareOpTimes({ts: Timestamp(2, 4), t: 2}, {ts: Timestamp(2, 2), t: 2}));
assert.eq(1, rs.compareOpTimes({ts: Timestamp(2, 0), t: 3}, {ts: Timestamp(3, 0), t: 2}));
})();
