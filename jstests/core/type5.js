(function() {
    "use strict";

    // This checks SERVER-20375 - Constrain JS method thisv
    //
    // Check to make sure we can't invoke methods on incorrect types, or on
    // prototypes of objects that aren't intended to have methods invoked on
    // them.

    assert.throws(function() {
        HexData(0, "aaaa").hex.apply({});
    }, [], "invoke method on object of incorrect type");
    assert.throws(function() {
        var x = HexData(0, "aaaa");
        x.hex.apply(10);
    }, [], "invoke method on incorrect type");
    assert.throws(function() {
        var x = HexData(0, "aaaa");
        x.hex.apply(x.__proto__);
    }, [], "invoke method on prototype of correct type");

})();
