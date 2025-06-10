// Tests that unknown fields are not allowed in $lookup.
import {assertErrorCode} from "jstests/aggregation/extras/utils.js";

const orders = db.orders;
const inventory = db.inventory;
orders.drop();
inventory.drop();

assert.commandWorked(orders.insert([
    {_id: 1, item: "almonds", price: 12, quantity: 2},
    {_id: 2, item: "pecans", price: 20, quantity: 1},
    {_id: 3}
]));

assert.commandWorked(inventory.insert([
    {_id: 1, sku: "almonds", description: "product 1", instock: 120},
    {_id: 2, sku: "bread", description: "product 2", instock: 80},
    {_id: 3, sku: "cashews", description: "product 3", instock: 60},
    {_id: 4, sku: "pecans", description: "product 4", instock: 70},
    {_id: 5, sku: null, description: "Incomplete"},
    {_id: 6}
]));

// For the assert with multiple error codes, SERVER-93055 changes the parse error codes to an IDL
// code, as these errors are now caught during IDL parsing. However, due to these tests being
// run across multiple versions, we need to allow both types of errors.

// TODO SERVER-106081: Remove ErrorCodes.FailedToParse.
assertErrorCode(orders, 
    [{$lookup: { 
                from: "inventory", 
                localField: "item", 
                foreignField: "sku", 
                as: "inventory_docs", 
                hi: "hi"
            }}], 
    [ErrorCodes.IDLUnknownField, ErrorCodes.FailedToParse]);
