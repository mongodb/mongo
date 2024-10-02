/**
 * Test that executing commands after collection.drop() returns the expected results.
 *
 * @tags: [
 *   # This test uses getNext explicitly, so is sensitive to cases where it is retried and/or run on
 *   # a secondary mongod
 *   assumes_standalone_mongod,
 *   does_not_support_stepdowns,
 *   requires_non_retryable_writes,
 *   # Those are required for testing mapReduce
 *   not_allowed_with_signed_security_token,
 *   requires_scripting
 * ]
 */

import {getWinningPlanFromExplain, isEofPlan} from "jstests/libs/analyze_plan.js";

const docs = [{a: 1}, {a: 2}, {a: 2}, {a: 3}];
const dbName = "validate_commands_after_coll_drop_db";
const collName = "validate_commands_after_coll_drop_db";

const mongo = db.getMongo();

/* The individual flavors of drop that we will be testing. */
const dropOperations = [
    {
        dropCommand: (function(db) {
            assert.commandWorked(db.runCommand({drop: collName}));
        })
    },
    {
        dropsDatabase: true,
        dropCommand: (function() {
            assert.commandWorked(mongo.getDB(dbName).dropDatabase());
        })
    }

];

/*
 * Individual items from this array correspond to distinct commands whose operation after
 * collection.drop() will be tested. The beforeDrop() function takes database and collection as
 * arguments and returns an object, such as a cursor, that is then passed as the third argument of
 * afterDrop().
 */
const commandValidatorsAfterCollDrop = [
    {
        afterDrop: (function(_, coll, __) {
            return coll.aggregate(
                           {$match: {a: 2}},
                           )
                       .toArray()
                       .length == 0;
        })
    },
    {
        afterDrop: (function(_, coll, __) {
            return coll.count({a: 2}) == 0;
        })

    },
    {
        afterDrop: (function(_, coll, __) {
            return coll.countDocuments({a: 2}) == 0;
        })
    },
    {
        afterDrop: (function(_, coll, __) {
            return coll.deleteOne({a: 3}).deletedCount == 0;
        })
    },
    {
        afterDrop: (function(_, coll, __) {
            return coll.deleteMany({a: 3}).deletedCount == 0;
        })
    },
    {
        afterDrop: (function(_, coll, __) {
            return coll.distinct("a").length == 0;
        })
    },
    {
        afterDrop: (function(_, coll, __) {
            return coll.estimatedDocumentCount() == 0;
        })
    },
    {
        afterDrop: (function(_, coll, __) {
            return coll.find({a: 2}).toArray().length == 0;
        })

    },
    // We also test find() with no condition in case it is handled differently.
    {
        afterDrop: (function(_, coll, __) {
            return coll.find().toArray().length == 0;
        })

    },
    // Test creating the cursor before the drop() with the default batch size.
    // No error expected since the entire result is populated prior to the drop().
    {
        beforeDrop: (function(_, coll) {
            return coll.find({a: 2});
        }),

        afterDrop: (function(_, __, cur) {
            return cur.toArray().length == 0;
        })
    },
    // Test a cursor with a batchSize of 1. The getMore after the drop will error out.
    {
        beforeDrop: (function(_, coll, __) {
            const cur = coll.find({a: 2}).batchSize(1);
            cur.next();
            return cur;
        }),

        afterDrop: (function(db, _, cur) {
            assert.commandFailedWithCode(
                db.runCommand("getMore", {getMore: cur._cursor._cursorid, collection: collName}),
                ErrorCodes.QueryPlanKilled);
            return true;
        })
    },
    {
        afterDrop: (function(_, coll, __) {
            return coll.find({a: 2}).batchSize(0).toArray().length == 0;
        })

    },
    {
        afterDrop: (function(_, coll, __) {
            return coll.find({a: 2}).batchSize(1).toArray().length == 0;
        })

    },
    {
        afterDrop: (function(_, coll, __) {
            return coll.find().limit(1).toArray().length == 0;
        })
    },
    {
        afterDrop: (function(_, coll, __) {
            return coll.find().skip(1).toArray().length == 0;
        })

    },
    {
        afterDrop: (function(_, coll, __) {
            return coll.find().sort({"a": 1}).toArray().length == 0;
        })

    },
    {
        afterDrop: (function(_, coll, __) {
            return coll.find().sort({"a": -1}).toArray().length == 0;
        })

    },
    {
        afterDrop: (function(_, coll, __) {
            return coll.findAndModify({query: {a: 2}, update: {$inc: {a: 1}}, upsert: true}) ==
                null;
        })
    },
    {
        afterDrop: (function(_, coll, __) {
            return coll.findOne({a: 2}) == null;
        })

    },
    {
        afterDrop: (function(_, coll, __) {
            return coll.findOneAndDelete({a: 2}) == null;
        })
    },
    {
        // We skip this test if we are using dropDatabase() as there will be no database to write
        // the ouput mapReduce collection to.
        requiresUndroppedDatabase: true,
        afterDrop: (function(db, coll) {
            coll.mapReduce(
                function() {
                    emit(this.a);
                },
                function(a) {
                    return Array.sum(a);
                },
                {out: "validate_commands_after_drop_map_reduce1"});
            return db.validate_commands_after_drop_map_reduce1.find().toArray().length == 0;
        })
    },
    {
        afterDrop: (function(_, coll, __) {
            return coll.replaceOne({a: 3}, {a: 5}).matchedCount == 0;
        })
    },
    {
        afterDrop: (function(_, coll, __) {
            return coll.update({a: 3}, {$set: {b: 3}}).nMatched == 0;
        })
    },

    {
        afterDrop: (function(_, coll, __) {
            return coll.updateOne({a: 3}, {$set: {b: 3}}).matchedCount == 0;
        })
    },
    {
        afterDrop: (function(_, coll, __) {
            return coll.updateMany({a: 3}, {$set: {b: 3}}).matchedCount == 0;
        })
    },

    //
    // The following operations continue to return after collection.drop():
    //

    {
        afterDrop: (function(_, coll) {
            const explain = assert.commandWorked(coll.find().explain());
            let winningPlan = getWinningPlanFromExplain(explain);
            assert(isEofPlan(db, winningPlan));
            return explain != null;
        })
    },
    /*
     * Calling mapReduce() before the drop(). The complete result can  still be retrieved after the
     * drop(). We skip this test if we are using dropDatabase() as there will  be no database to
     * write the ouput mapReduce collection to.
     */
    {
        requiresUndroppedDatabase: true,
        beforeDrop: (function(_, coll) {
            coll.mapReduce(
                function() {
                    emit(this._id, this.a);
                },
                function(key, values) {
                    return Array.sum(values);
                },
                {out: "validate_commands_after_drop_map_reduce2"});
        }),
        afterDrop: (function(db, _, __) {
            return db.validate_commands_after_drop_map_reduce2.find()
                       .batchSize(0)
                       .toArray()
                       .length > 0;
        })
    },
    {
        afterDrop: (function(_, coll) {
            return Object.keys(coll.stats()).length > 0;
        })

    },

];

for (const dropOperation of dropOperations) {
    for (const validator of commandValidatorsAfterCollDrop) {
        assert.commandWorked(mongo.getDB(dbName).dropDatabase());
        const db = mongo.getDB(dbName);
        const coll = db.getCollection(collName);

        assert.commandWorked(coll.insertMany(docs));

        const cur = validator.beforeDrop ? validator.beforeDrop(db, coll) : null;

        // Note that most Validators have no beforeDrop, so they run entirely on a non-existent
        // collection.
        dropOperation.dropCommand(db);

        if (dropOperation.dropsDatabase && validator.requiresUndroppedDatabase) {
            continue;
        }

        assert(validator.afterDrop(db, coll, cur),
               `Unexpected result from afterDrop: ${validator.afterDrop}, dropCommand: ${
                   dropOperation.dropCommand};`);
    }
}
