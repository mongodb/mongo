assert.soon = function(func) {
    if (typeof (func) == "string") {
        eval(func);
    } else {
        func();
    }
};

doassert = function() { /* noop */ };
