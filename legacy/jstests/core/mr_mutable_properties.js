// See SERVER-9448
// Test argument and receiver (aka 'this') objects and their children can be mutated
// in Map, Reduce and Finalize functions

var collection = db.mrMutableReceiver;
collection.drop();
collection.insert({a:1});

var map = function() {
    // set property on receiver
    this.feed = {beef:1};

    // modify property on receiever
    this.a = {cake:1};
    emit(this._id, this.feed);
    emit(this._id, this.a);
}

var reduce = function(key, values) {
    // set property on receiver
    this.feed = {beat:1};

    // set property on key arg
    key.fed = {mochi:1};

    // push properties onto values array arg
    values.push(this.feed);
    values.push(key.fed);

    // modify each value in the (modified) array arg
    values.forEach(function(val) { val.mod = 1; });
    return {food:values};
}

var finalize = function(key, values) {
    // set property on receiver
    this.feed = {ice:1};

    // set property on key arg
    key.fed = {cream:1};

    // push properties onto values array arg
    printjson(values);
    values.food.push(this.feed);
    values.food.push(key.fed);

    // modify each value in the (modified) array arg
    values.food.forEach(function(val) { val.mod = 1; });
    return values;
}

var mr = collection.mapReduce(map, reduce, {finalize: finalize, out: {inline: 1}});
printjson(mr);

// verify mutated properties exist (order dictated by emit sequence and properties added)
assert.eq(mr.results[0].value.food[0].beef, 1);
assert.eq(mr.results[0].value.food[1].cake, 1);
assert.eq(mr.results[0].value.food[2].beat, 1);
assert.eq(mr.results[0].value.food[3].mochi, 1);
assert.eq(mr.results[0].value.food[4].ice, 1);
assert.eq(mr.results[0].value.food[5].cream, 1);
mr.results[0].value.food.forEach(function(val) { assert.eq(val.mod, 1); });
