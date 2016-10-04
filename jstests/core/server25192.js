(function() {

    "use strict";

    var x = {};

    assert.doesNotThrow(function() {
        Object.extend(x, {a: null}, true);
    }, [], "Extending an object with a null field does not throw");

    assert.eq(x.a, null);
}());
