// SERVER-11781 Don't crash when converting deeply nested or cyclical JS objects to BSON.

function test() {
    function assertTooBig(obj) {
        // This used to crash rather than throwing an exception.
        assert.throws(function() {
            Object.bsonsize(obj);
        });
    }

    function assertNotTooBig(obj) {
        assert.doesNotThrow(function() {
            Object.bsonsize(obj);
        });
    }

    function objWithDepth(depth) {
        var out = 1;
        while (depth--) {
            out = {o: out};
        }
        return out;
    }

    function arrayWithDepth(depth) {
        var out = 1;
        while (depth--) {
            out = [out];
        }
        return out;
    }

    assertNotTooBig({});
    assertNotTooBig({array: []});

    var objCycle = {};
    objCycle.cycle = objCycle;
    assertTooBig(objCycle);

    var arrayCycle = [];
    arrayCycle.push(arrayCycle);
    assertTooBig({array: arrayCycle});

    var objDepthLimit = 150;
    assertNotTooBig(objWithDepth(objDepthLimit - 1));
    assertTooBig(objWithDepth(objDepthLimit));

    var arrayDepthLimit = objDepthLimit - 1;  // one lower due to wrapping object
    assertNotTooBig({array: arrayWithDepth(arrayDepthLimit - 1)});
    assertTooBig({array: arrayWithDepth(arrayDepthLimit)});
}

// test in shell
test();

// test on server
db.depth_limit.drop();
db.depth_limit.insert({});
db.depth_limit.find({$where: test}).itcount();  // itcount ensures that cursor is executed on server
