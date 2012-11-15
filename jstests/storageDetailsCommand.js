db.jstests_commands.drop();
db.createCollection("jstests_commands");

t = db.jstests_commands;

for (var i = 0; i < 3000; ++i) {
    t.insert({i: i, d: i % 13});
}

function test() {
    var result = t.diskStorageStats({numberOfSlices: 100});
    if (result["bad cmd"]) {
        print("storageDetails command not available: skipping");
        return;
    }

    assert.commandWorked(result);

    function checkDiskStats(data) {
        assert(isNumber(data.extentHeaderBytes));
        assert(isNumber(data.recordHeaderBytes));
        assert(isNumber(data.numEntries));
        assert(data.bsonBytes instanceof NumberLong);
        assert(data.recBytes instanceof NumberLong);
        assert(data.onDiskBytes instanceof NumberLong);
        assert(isNumber(data.outOfOrderRecs));
        assert(isNumber(data.characteristicCount));
        assert(isNumber(data.characteristicAvg));
        assert(data.freeRecsPerBucket instanceof Array);
    }

    assert(result.extents && result.extents instanceof Array);

    var extents = result.extents;

    for (var i = 0; i < extents.length; ++i) {
        assert(isObject(extents[i]));
        assert.neq(extents[i], null);
        assert(extents[i].range instanceof Array);
        assert.eq(extents[i].range.length, 2);
        assert.eq(extents[i].isCapped, false);
        checkDiskStats(extents[i]);
        assert(extents[i].slices instanceof Array);
        for (var c = 0; c < extents[i].slices[c]; ++c) {
            assert(isObject(extents[i].slices[c]));
            assert.neq(extents[i].slices[c], null);
            checkStats(extents[i].slices[c]);
        }
    }

    result = t.pagesInRAM({numberOfSlices: 100});
    assert(result.ok);

    assert(result.extents instanceof Array);
    var extents = result.extents;

    for (var i = 0; i < result.extents.length; ++i) {
        assert(isObject(extents[i]));
        assert.neq(extents[i], null);
        assert(isNumber(extents[i].pageBytes));
        assert(isNumber(extents[i].onDiskBytes));
        assert(isNumber(extents[i].inMem));

        assert(extents[i].slices instanceof Array);
        for (var c = 0; c < extents[i].slices.length; ++c) {
            assert(isNumber(extents[i].slices[c]));
        }
    }

    function checkErrorConditions(helper) {
        var result = helper.apply(t, [{extent: 'a'}]);
        assert.commandFailed(result);
        assert(result.errmsg.match(/extent.*must be a number/));

        result = helper.apply(t, [{range: [2, 4]}]);
        assert.commandFailed(result);
        assert(result.errmsg.match(/range is only allowed.*extent/));

        result = helper.apply(t, [{extent: 3, range: [3, 'a']}]);
        assert.commandFailed(result);
        assert(result.errmsg.match(/must be an array.*numeric elements/));

        result = helper.apply(t, [{granularity: 'a'}]);
        assert.commandFailed(result);
        assert(result.errmsg.match(/granularity.*number/));

        result = helper.apply(t, [{numberOfSlices: 'a'}]);
        assert.commandFailed(result);
        assert(result.errmsg.match(/numberOfSlices.*number/));

        result = helper.apply(t, [{extent: 100}]);
        assert.commandFailed(result);
        assert(result.errmsg.match(/extent.*does not exist/));
    }

    checkErrorConditions(t.diskStorageStats);
    checkErrorConditions(t.pagesInRAM);
}
