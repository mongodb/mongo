/** Demonstrate that mapReduce can accept functions represented by strings.
 * Some drivers do not have a type which represents a Javascript function. These languages represent
 * the arguments to mapReduce as strings.
 */

(function() {
    "use strict";

    var col = db.function_string_representations;
    col.drop();
    assert.writeOK(col.insert({
        _id: "abc123",
        ord_date: new Date("Oct 04, 2012"),
        status: 'A',
        price: 25,
        items: [{sku: "mmm", qty: 5, price: 2.5}, {sku: "nnn", qty: 5, price: 2.5}]
    }));

    var mapFunction = "function() {emit(this._id, this.price);}";
    var reduceFunction = "function(keyCustId, valuesPrices) {return Array.sum(valuesPrices);}";
    assert.commandWorked(col.mapReduce(mapFunction, reduceFunction, {out: "map_reduce_example"}));

    // Provided strings may end with semicolons and/or whitespace
    mapFunction += " ; ";
    reduceFunction += " ; ";
    assert.commandWorked(col.mapReduce(mapFunction, reduceFunction, {out: "map_reduce_example"}));

    // $where exhibits the same behavior
    var whereFunction = "function() {return this.price === 25;}";
    assert.eq(1, col.find({$where: whereFunction}).itcount());

    whereFunction += ";";
    assert.eq(1, col.find({$where: whereFunction}).itcount());

    // db.eval does not need to be tested, as it accepts code fragments, not functions.
    // system.js does not need to be tested, as its contents types' are preserved, and
    // strings are not promoted into functions.
})();
