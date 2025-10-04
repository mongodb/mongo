/**
 * Demonstrate that mapReduce can accept functions represented by strings.
 * Some drivers do not have a type which represents a Javascript function. These languages represent
 * the arguments to mapReduce as strings.
 * @tags: [
 *   # The test runs commands that are not allowed with security token: mapReduce.
 *   not_allowed_with_signed_security_token,
 *   does_not_support_stepdowns,
 *   uses_map_reduce_with_temp_collections,
 *   # Uses $where operator
 *   requires_scripting,
 * ]
 */

const col = db.function_string_representations;
const out = db.map_reduce_example;
col.drop();
assert.commandWorked(
    col.insert({
        _id: "abc123",
        ord_date: new Date("Oct 04, 2012"),
        status: "A",
        price: 25,
        items: [
            {sku: "mmm", qty: 5, price: 2.5},
            {sku: "nnn", qty: 5, price: 2.5},
        ],
    }),
);

let mapFunction = "function() {emit(this._id, this.price);}";
let reduceFunction = "function(keyCustId, valuesPrices) {return Array.sum(valuesPrices);}";
out.drop();
assert.commandWorked(col.mapReduce(mapFunction, reduceFunction, {out: {merge: "map_reduce_example"}}));

// Provided strings may end with semicolons and/or whitespace
mapFunction += " ; ";
reduceFunction += " ; ";
out.drop();
assert.commandWorked(col.mapReduce(mapFunction, reduceFunction, {out: {merge: "map_reduce_example"}}));

// $where exhibits the same behavior
let whereFunction = "function() {return this.price === 25;}";
assert.eq(1, col.find({$where: whereFunction}).itcount());

whereFunction += ";";
assert.eq(1, col.find({$where: whereFunction}).itcount());

// system.js does not need to be tested, as its contents types' are preserved, and
// strings are not promoted into functions.
