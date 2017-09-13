// SERVER-5044 Standard deviation expression operators

var t = db.server5044;

function test(data, popExpected, sampExpected) {
    t.drop();
    assert.writeOK(t.insert({}));  // need one document to ensure we get output

    for (var i = 0; i < data.length; i++)
        assert.writeOK(t.insert({num: data[i]}));

    var res = t.aggregate({
                   $group: {
                       _id: 1,
                       pop: {$stdDevPop: '$num'},
                       samp: {$stdDevSamp: '$num'},
                   }
               }).next();

    if (popExpected === null) {
        assert.isnull(res.pop);
    } else {
        assert.close(res.pop, popExpected, '', 10 /*decimal places*/);
    }

    if (sampExpected === null) {
        assert.isnull(res.samp);
    } else {
        assert.close(res.samp, sampExpected, '', 10 /*decimal places*/);
    }
}

test([], null, null);
test([1], 0, null);
test([1, 1], 0, 0);

test(['a'], null, null);
test([1, 'a'], 0, null);
test([1, 'a', 1], 0, 0);

test([1, 2], .5, Math.sqrt(.5));
test([1, 2, 3], Math.sqrt(2 / 3), 1);

// test from http://en.wikipedia.org/wiki/Algorithms_for_calculating_variance#Example
test([4, 7, 13, 16], Math.sqrt(22.5), Math.sqrt(30));
test([1e8 + 4, 1e8 + 7, 1e8 + 13, 1e8 + 16], Math.sqrt(22.5), Math.sqrt(30));
test([1e9 + 4, 1e9 + 7, 1e9 + 13, 1e9 + 16], Math.sqrt(22.5), Math.sqrt(30));
test([1e10 + 4, 1e10 + 7, 1e10 + 13, 1e10 + 16], Math.sqrt(22.5), Math.sqrt(30));
