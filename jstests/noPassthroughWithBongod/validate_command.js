// Tests that the basic values returned from the validate command are correct

(function() {
    // Set the number of documents to insert
    var count = 10;

    function testValidate(output) {
        assert.eq(output.nrecords, count, "validate returned an invalid count");
        assert.eq(output.nIndexes, 3, "validate returned an invalid number of indexes");

        var indexNames = output.keysPerIndex;

        for (var i in indexNames) {
            if (!indexNames.hasOwnProperty(i))
                continue;
            assert.eq(indexNames[i], count, "validate returned an invalid number of indexes");
        }
    }

    // Test to confirm that validate is working as expected.

    // SETUP DATA
    t = db.jstests_validate;
    t.drop();

    for (var i = 0; i < count; i++) {
        t.insert({x: i});
    }

    t.ensureIndex({x: 1}, {name: "forward"});
    t.ensureIndex({x: -1}, {name: "reverse"});

    // TEST NORMAL VALIDATE
    var output = t.validate();
    testValidate(output);

    // TEST FULL
    var output = t.validate({full: true});
    testValidate(output);
}());