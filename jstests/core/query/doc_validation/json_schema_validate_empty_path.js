/**
 * Verify that a collection validator with a $jsonSchema referencing empty field names behaves
 * correctly.
 */

import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";

const coll = assertDropAndRecreateCollection(db, jsTestName());
assert.commandWorked(db.runCommand({
    collMod: coll.getName(),
    validator: {
        "$jsonSchema": {
            "bsonType": "object",
            "required": [""],
            "properties": {"": {"bsonType": "string", "description": "Field of type String"}}
        }
    }
}));
assert.commandFailedWithCode(coll.insert({a: 1}), ErrorCodes.DocumentValidationFailure);
