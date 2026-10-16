/**
 * Enumerates the commands that reach a single namespace, each tagged with the ActionTypes the
 * server requires on that namespace to run it.
 *
 * A namespace is not protected by locking down the obvious command that names it: 'find' is also
 * reachable through '$lookup', an insert is also reachable through 'applyOps' and '$out', and a
 * definition change is also reachable through 'collMod' or a view. A test that asserts an
 * authorization rule on a namespace has to cover every one of those paths, and the paths are the
 * same for every namespace, so they are enumerated here once.
 *
 * Entries have the form:
 *
 *   {name, actionTypes, db, command, shardOnly}
 *
 * 'actionTypes' is the set of actions the command requires *on the namespace under test*, taken
 * from the server's own authorization checks. It is a set, not a single action: 'update' with
 * 'upsert: true' requires {update, insert}, '$out' requires {insert, remove}, and a command that
 * names two namespaces (rename, cloneCollectionAsCapped) requires a different set on each side, so
 * only the side that touches the namespace under test is listed. Note also that some commands
 * require actions on a *second* namespace that are not listed here — creating a view over the
 * namespace needs 'createCollection' on the new view, but only read access on the namespace itself.
 *
 * 'db' is the database the command must be run against, which is not necessarily the database
 * holding the namespace ('bulkWrite', 'applyOps' and 'renameCollection' run against 'admin').
 * 'shardOnly' marks commands that do not exist on a router.
 *
 * Entries carry commands only; the caller decides what to assert about running them, since a
 * command that is denied on one namespace may be expected to succeed, or to fail with a different
 * code, on another.
 *
 * Example:
 *
 *   import {CollectionActions} from "jstests/auth/lib/collection_actions.js";
 *
 *   const spec = {db: "config", coll: "myStateDocuments", document: doc, id: doc._id};
 *   for (const {name, db, command} of CollectionActions.all(spec)) {
 *       assert.commandFailedWithCode(conn.getDB(db).runCommand(command), ErrorCodes.Unauthorized);
 *   }
 */

// Names of the helper namespaces that the rename and clone paths need as their other side, and the
// view they define. These are arbitrary; a caller only needs to override them if it also uses a
// collection by that name.
const kDefaults = {
    db: "config",
    scratchCollection: "collectionActionsScratch",
    viewName: "collectionActionsView",
    validator: {_id: {$exists: true}},
};

function resolve(spec) {
    const resolved = Object.assign({}, kDefaults, spec);
    assert(resolved.coll, "CollectionActions requires a 'coll': " + tojson(spec));
    assert(resolved.document, "CollectionActions requires a 'document' to write: " + tojson(spec));
    assert(resolved.id !== undefined,
           "CollectionActions requires an 'id' to filter on: " + tojson(spec));
    resolved.qualifiedNs = `${resolved.db}.${resolved.coll}`;
    resolved.qualifiedScratchNs = `${resolved.db}.${resolved.scratchCollection}`;
    return resolved;
}

/**
 * Every command that reaches the namespace, in one flat list. The action-type accessors on
 * 'CollectionActions' are filters over this.
 */
function accessPaths(spec) {
    const {
        db,
        coll,
        qualifiedNs,
        qualifiedScratchNs,
        scratchCollection,
        viewName,
        validator,
        document,
        id,
    } = resolve(spec);

    return [
        // Reads. Each of these returns the contents of the namespace, either as the command's own
        // target or as a secondary namespace of a pipeline.
        {name: "find", actionTypes: ["find"], db, command: {find: coll, filter: {}}},
        {name: "count", actionTypes: ["find"], db, command: {count: coll, query: {}}},
        {name: "distinct", actionTypes: ["find"], db, command: {distinct: coll, key: "_id"}},
        {
            name: "aggregate with an empty pipeline",
            actionTypes: ["find"],
            db,
            command: {aggregate: coll, pipeline: [], cursor: {}},
        },
        {
            name: "aggregate with a pipeline",
            actionTypes: ["find"],
            db,
            command: {aggregate: coll, pipeline: [{$match: {}}, {$project: {_id: 1}}], cursor: {}},
        },
        {
            name: "aggregate naming the namespace via $unionWith",
            actionTypes: ["find"],
            db,
            command: {
                aggregate: 1,
                pipeline: [{$documents: [{}]}, {$unionWith: {coll: coll, pipeline: []}}],
                cursor: {},
            },
        },
        {
            name: "aggregate naming the namespace via $lookup",
            actionTypes: ["find"],
            db,
            command: {
                aggregate: 1,
                pipeline: [
                    {$documents: [{}]},
                    {$lookup: {from: coll, localField: "_id", foreignField: "_id", as: "joined"}},
                ],
                cursor: {},
            },
        },
        {
            name: "aggregate naming the namespace via $graphLookup",
            actionTypes: ["find"],
            db,
            command: {
                aggregate: 1,
                pipeline: [
                    {$documents: [{_id: 1}]},
                    {
                        $graphLookup: {
                            from: coll,
                            startWith: "$_id",
                            connectFromField: "_id",
                            connectToField: "_id",
                            as: "reached",
                        },
                    },
                ],
                cursor: {},
            },
        },
        {
            name: "mapReduce",
            actionTypes: ["find"],
            db,
            command: {
                mapReduce: coll,
                map: function () {
                    emit(this._id, 1);
                },
                reduce: function (k, v) {
                    return 1;
                },
                out: {inline: 1},
            },
        },
        {
            // Only read access on the namespace is required here; 'createCollection' is required on
            // the view being defined, which is a different namespace. A view would let a later find
            // read the namespace indirectly.
            name: "create view over the namespace",
            actionTypes: ["find"],
            db,
            command: {create: viewName, viewOn: coll, pipeline: []},
        },

        // Inserts.
        {
            name: "insert",
            actionTypes: ["insert"],
            db,
            command: {insert: coll, documents: [document]},
        },
        {
            name: "bulkWrite insert",
            actionTypes: ["insert"],
            db: "admin",
            command: {
                bulkWrite: 1,
                ops: [{insert: 0, document: document}],
                nsInfo: [{ns: qualifiedNs}],
            },
        },
        {
            name: "applyOps insert",
            actionTypes: ["insert"],
            db: "admin",
            shardOnly: true,
            command: {applyOps: [{op: "i", ns: qualifiedNs, o: document}]},
        },

        // Updates. An upsert also requires 'insert', since it can create the document.
        {
            name: "update with upsert",
            actionTypes: ["update", "insert"],
            db,
            command: {
                update: coll,
                updates: [{q: {_id: id}, u: document, multi: false, upsert: true}],
            },
        },
        {
            name: "multi update",
            actionTypes: ["update"],
            db,
            command: {update: coll, updates: [{q: {}, u: {$set: {touched: true}}, multi: true}]},
        },
        {
            // findAndModify always requires 'find', because it returns the document it acted on.
            name: "findAndModify with update",
            actionTypes: ["find", "update", "insert"],
            db,
            command: {findAndModify: coll, query: {_id: id}, update: document, upsert: true},
        },
        {
            name: "bulkWrite update",
            actionTypes: ["update", "insert"],
            db: "admin",
            command: {
                bulkWrite: 1,
                ops: [{update: 0, filter: {_id: id}, updateMods: document, upsert: true}],
                nsInfo: [{ns: qualifiedNs}],
            },
        },
        {
            name: "applyOps update",
            actionTypes: ["update"],
            db: "admin",
            shardOnly: true,
            command: {applyOps: [{op: "u", ns: qualifiedNs, o: document, o2: {_id: id}}]},
        },

        // Removes.
        {
            name: "delete of all documents",
            actionTypes: ["remove"],
            db,
            command: {delete: coll, deletes: [{q: {}, limit: 0}]},
        },
        {
            name: "delete of a single document",
            actionTypes: ["remove"],
            db,
            command: {delete: coll, deletes: [{q: {_id: id}, limit: 1}]},
        },
        {
            name: "findAndModify with remove",
            actionTypes: ["find", "remove"],
            db,
            command: {findAndModify: coll, query: {_id: id}, remove: true},
        },
        {
            name: "bulkWrite delete",
            actionTypes: ["remove"],
            db: "admin",
            command: {
                bulkWrite: 1,
                ops: [{delete: 0, filter: {_id: id}, multi: false}],
                nsInfo: [{ns: qualifiedNs}],
            },
        },
        {
            name: "applyOps delete",
            actionTypes: ["remove"],
            db: "admin",
            shardOnly: true,
            command: {applyOps: [{op: "d", ns: qualifiedNs, o: {_id: id}}]},
        },

        // Output stages. $out replaces the whole collection, so it holds 'remove' as well as
        // 'insert'; $merge's actions depend on its mode pair, and 'replace'/'insert' is the mode
        // that holds both write actions.
        {
            name: "aggregate writing via $out",
            actionTypes: ["insert", "remove"],
            db,
            command: {aggregate: 1, pipeline: [{$documents: [document]}, {$out: coll}], cursor: {}},
        },
        {
            name: "aggregate writing via $merge",
            actionTypes: ["insert", "update"],
            db,
            command: {
                aggregate: 1,
                pipeline: [
                    {$documents: [document]},
                    {$merge: {into: coll, whenMatched: "replace", whenNotMatched: "insert"}},
                ],
                cursor: {},
            },
        },

        // Definition changes. collMod can attach a validator that silently rewrites what a later
        // legitimate write to the namespace is allowed to store, and on a view it redefines what
        // the view reads.
        {
            name: "collMod",
            actionTypes: ["collMod"],
            db,
            command: {collMod: coll, validator: validator},
        },
        {
            name: "drop",
            actionTypes: ["dropCollection"],
            db,
            command: {drop: coll},
        },
        {
            // Capping the namespace in place is a definition change like collMod: a later
            // legitimate write to it can then be silently dropped to keep the collection in size.
            name: "convertToCapped",
            actionTypes: ["convertToCapped"],
            db,
            command: {convertToCapped: coll, size: 100000},
        },
        {
            name: "cloneCollectionAsCapped reading the namespace",
            actionTypes: ["find"],
            db,
            shardOnly: true,
            command: {cloneCollectionAsCapped: coll, toCollection: scratchCollection, size: 100000},
        },
        {
            name: "cloneCollectionAsCapped writing the namespace",
            actionTypes: ["insert", "createIndex", "convertToCapped"],
            db,
            shardOnly: true,
            command: {cloneCollectionAsCapped: scratchCollection, toCollection: coll, size: 100000},
        },

        // Renaming moves the namespace's contents somewhere readable, or moves attacker-controlled
        // contents into the namespace. 'renameCollectionSameDB' on the database is an alternative
        // to the source-side actions below, so it is not listed as required on the namespace.
        {
            name: "renameCollection away from the namespace",
            actionTypes: ["find", "dropCollection"],
            db: "admin",
            command: {renameCollection: qualifiedNs, to: qualifiedScratchNs, dropTarget: true},
        },
        {
            name: "renameCollection onto the namespace",
            actionTypes: ["insert", "createIndex", "dropCollection"],
            db: "admin",
            command: {renameCollection: qualifiedScratchNs, to: qualifiedNs, dropTarget: true},
        },
    ];
}

/**
 * Only the paths requiring at least one of 'actionTypes' on the namespace. A path that requires
 * several actions is returned for each of them: '$out' is both an 'insert' path and a 'remove' one.
 */
function forActionTypes(actionTypes, spec) {
    return accessPaths(spec).filter((entry) =>
        entry.actionTypes.some((action) => actionTypes.includes(action)),
    );
}

export const CollectionActions = {
    all: accessPaths,
    forActionTypes,

    find: (spec) => forActionTypes(["find"], spec),
    insert: (spec) => forActionTypes(["insert"], spec),
    update: (spec) => forActionTypes(["update"], spec),
    remove: (spec) => forActionTypes(["remove"], spec),
    collMod: (spec) => forActionTypes(["collMod"], spec),
    convertToCapped: (spec) => forActionTypes(["convertToCapped"], spec),
    createIndex: (spec) => forActionTypes(["createIndex"], spec),
    dropCollection: (spec) => forActionTypes(["dropCollection"], spec),
};
