// server-9444 support disk storage of intermediate results in aggregation

var t = db.server9444;
t.drop();

var memoryLimitMB = 100;

function loadData() {
    var bigStr = Array(1024*1024 + 1).toString(); // 1MB of ','
    for (var i = 0; i < 101; i++)
        t.insert({_id: i, bigStr: i + bigStr, random: Math.random()});

    assert.gt(t.stats().size, memoryLimitMB * 1024*1024);
}
loadData();

function test(pipeline, outOfMemoryCode) {
    // ensure by default we error out if exceeding memory limit
    var res = t.runCommand('aggregate', {pipeline: pipeline});
    assert.commandFailed(res);
    assert.eq(res.code, outOfMemoryCode);

    // ensure allowDiskUsage: false does what it says
    var res = t.runCommand('aggregate', {pipeline: pipeline, allowDiskUsage: false});
    assert.commandFailed(res);
    assert.eq(res.code, outOfMemoryCode);

    // allowDiskUsage only supports bool. In particular, numbers aren't allowed.
    var res = t.runCommand('aggregate', {pipeline: pipeline, allowDiskUsage: 1});
    assert.commandFailed(res);
    assert.eq(res.code, 16949);

    // ensure we work when allowingDiskUsage === true
    var res = t.aggregateCursor(pipeline, {allowDiskUsage: true});
    assert.eq(res.itcount(), t.count()); // all tests output one doc per input doc
}

var groupCode = 16945;
var sortCode = 16819;
var sortLimitCode = 16820;

test([{$group: {_id: '$_id', bigStr: {$first: '$bigStr'}}}], groupCode);

// sorting with _id would use index which doesn't require extsort
test([{$sort: {random: 1}}], sortCode);
test([{$sort: {bigStr: 1}}], sortCode); // big key and value

// make sure sort + large limit won't crash the server (SERVER-10136)
test([{$sort: {bigStr: 1}}, {$limit:1000*1000*1000}], sortLimitCode);

// test combining two extSorts in both same and different orders
test([{$group: {_id: '$_id', bigStr: {$first: '$bigStr'}}}, {$sort: {_id:1}}], groupCode);
test([{$group: {_id: '$_id', bigStr: {$first: '$bigStr'}}}, {$sort: {_id:-1}}], groupCode);
test([{$group: {_id: '$_id', bigStr: {$first: '$bigStr'}}}, {$sort: {random:1}}], groupCode);
test([{$sort: {random:1}}, {$group: {_id: '$_id', bigStr: {$first: '$bigStr'}}}], sortCode);

// don't leave large collection laying around
t.drop();
