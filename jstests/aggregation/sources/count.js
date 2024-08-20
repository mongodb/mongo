import {assertErrCodeAndErrMsgContains, assertErrorCode} from "jstests/aggregation/extras/utils.js";

const coll = db.aggregation_count;
coll.drop();

{
    const pipeline = [{"$count": "_id"}];
    assertErrorCode(coll, pipeline, 9039800);
    assertErrCodeAndErrMsgContains(coll, pipeline, 9039800, "the count field cannot be '_id'");
}

// Tests invalid specs for $count.
assertErrorCode(coll, [{"$count": 1}], 40156);
assertErrorCode(coll, [{"$count": ""}], 40157);
assertErrorCode(coll, [{"$count": "$x"}], 40158);
assertErrorCode(coll, [{"$count": "te\u0000st"}], 40159);
assertErrorCode(coll, [{"$count": "test.string"}], 40160);

assert.commandWorked(coll.insertMany([...Array(1000).keys()].map(i => ({a: i, condition: i % 2}))));

{
    const pipeline = [{"$count": "test"}];
    const result = (coll.aggregate(pipeline).toArray());
    assert.eq([{"test": 1000}], result);
}

{
    const pipeline = [{"$count": "myCount"}];
    const result = (coll.aggregate(pipeline).toArray());
    assert.eq([{"myCount": 1000}], result);
}

{
    const pipeline = [{"$count": "quantity"}];
    const result = (coll.aggregate(pipeline).toArray());
    assert.eq([{"quantity": 1000}], result);
}

{
    const pipeline = [{$match: {condition: 1}}, {$count: "quantity"}];
    const result = (coll.aggregate(pipeline).toArray());
    assert.eq([{"quantity": 500}], result);
}
