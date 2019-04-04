/**
 * Tests that numbers that are equivalent but have different types are grouped together.
 */
(function() {
    "use strict";
    const coll = db.coll;

    coll.drop();

    assert.writeOK(coll.insert({key: new NumberInt(24), value: new NumberInt(75)}));
    assert.writeOK(coll.insert({key: new NumberLong(24), value: new NumberLong(100)}));
    assert.writeOK(coll.insert({key: 24, value: 36}));

    assert.writeOK(coll.insert({key: new NumberInt(42), value: new NumberInt(75)}));
    assert.writeOK(coll.insert({key: new NumberLong(42), value: new NumberLong(100)}));
    assert.writeOK(coll.insert({key: 42, value: 36}));

    const result1 = coll.aggregate({$group: {_id: "$key", perc_result: {$percentile: {"value":"$value","perc":20}}}}).toArray();

    assert.eq(result1.length, 2, tojson(result1));

    assert.eq(result1[0].perc_result, 39.900000000000006, tojson(result1));
    assert.eq(result1[1].perc_result, 39.900000000000006, tojson(result1));
    coll.drop();
    
    assert.writeOK(coll.insert({temperature: 18,switch:1}));
    assert.writeOK(coll.insert({temperature: 10,switch:0}));
    assert.writeOK(coll.insert({temperature: 10,switch:0}));
    assert.writeOK(coll.insert({temperature: 10,switch:0}));
    assert.writeOK(coll.insert({temperature: 10,switch:1}));
    assert.writeOK(coll.insert({temperature: 20,switch:1}));
    assert.writeOK(coll.insert({temperature: 25,switch:1}));
    assert.writeOK(coll.insert({temperature: 30,switch:1}));
    assert.writeOK(coll.insert({temperature: 35,switch:1}));

    const result2 = db.coll.aggregate(
    [
        {
            '$project': {
                'valid_temp': {
                    '$cond': {
                        if: {'$eq': ['$switch', 1]},
                        then: '$temperature',
                        else: null
                    }
                },
            }
        },
        {
            "$group": {
                _id: null, 
                perc_result: {
                    $percentile: {
                        "value":"$valid_temp",
                        "perc":70}
                }
            }
        }
    ]).toArray();

    assert.eq(result2[0]['perc_result'], 28.499999999999996, tojson(result2));    

}());
