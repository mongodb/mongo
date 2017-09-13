(function() {
    'use strict';

    var o = new ObjectId();
    assert(o.getTimestamp);

    var a = new ObjectId("4c17f616a707427266a2801a");
    var b = new ObjectId("4c17f616a707428966a2801c");
    assert.eq(a.getTimestamp(), b.getTimestamp());
})();
