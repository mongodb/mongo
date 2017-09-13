(function() {
    "use strict";

    // SERVER-20319 Min/MaxKey check type of singleton
    //
    // make sure swapping min/max key's prototype doesn't blow things up

    assert.throws(function() {
        MinKey().__proto__.singleton = 1000;
        MinKey();
    }, [], "make sure manipulating MinKey's proto is safe");

    assert.throws(function() {
        MaxKey().__proto__.singleton = 1000;
        MaxKey();
    }, [], "make sure manipulating MaxKey's proto is safe");
})();
