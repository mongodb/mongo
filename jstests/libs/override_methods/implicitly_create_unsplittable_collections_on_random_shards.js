/**
 * Loading this file overrides the following methods in order to create unsplittable collections on
 * random shards implicitly:
 *  - DB.prototype.createCollection()
 *  - DB.prototype.getCollection()
 *  - Mongo.prototype.runCommand()
 *
 * TODO: SERVER-83396 Get rid of this file once we can set the same behaviour through a feature flag
 */

import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {OverrideHelpers} from "jstests/libs/override_methods/override_helpers.js";
import {ShardingOverrideCommon} from "jstests/libs/override_methods/shard_collection_util.js";

// Save a reference to the original methods to be called by the overrides below.
var originalRunCommand = DB.prototype.runCommand;
var originalGetCollection = DB.prototype.getCollection;
var originalInsert = DBCollection.prototype.insert;

const kUnsupportedOptions = new Set([
    'capped',
    'autoIndexId',
    'idIndex',
    'size',
    'max',
    'storageEngine',
    'validator',
    'validationLevel',
    'validationAction',
    'indexOptionDefaults',
    'viewOn',
    'pipeline',
    'collation',
    'changeStreamPreAndPostImages',
    'clusteredIndex',
    'expireAfterSeconds',
    'encryptedFields',
    'temp',
    'flags',
    'apiStrict',
    'apiVersion'
]);

/**
 * Override createCollection method to anticipate an implicit collection creation by creating it on
 * getCollection whether the collection doesn't exist yet.
 */
DB.prototype.createCollection = function(collName, opts) {
    const options = opts || {};

    // Call original create method if either its not called from mongos or the given options are not
    // supported
    let unsupportedOption = false;
    const optNames = Object.keys(options);
    optNames.forEach((optName) => {
        if (kUnsupportedOptions.has(optName)) {
            unsupportedOption = true;
        }
    })

    if (unsupportedOption || !FixtureHelpers.isMongos(this) ||
        ShardingOverrideCommon.nssCanBeTrackedByShardingCatalog(collName)) {
        // Because runCommand is overridden, using the original createCollection command will still
        // create an unsplittable collection on a random shard. Instead, we use the original
        // runCommand directly.
        var cmd = {create: collName};
        Object.extend(cmd, options);
        return originalRunCommand.apply(this, [cmd]);
    }

    let res = ShardingOverrideCommon.createUnsplittableCollectionOnRandomShard(
        {db: this, collName: collName, opts: options});

    if (!res.ok && res.code === ErrorCodes.AlreadyInitialized) {
        // This can happen if we created the collection on getCollection and then chose a different
        // data shard when we created it above. However, we want to ensure that this is what
        // happened and the AlreadyInitialized error wasn't for some other reason so we issue the
        // command without specifying a dataShard.
        delete options.dataShard;
        var cmd = {create: collName};
        Object.extend(cmd, options);
        res = originalRunCommand.apply(this, [cmd]);
    }
    return res;
};

/**
 * Override getCollection method to anticipate an implicit collection creation by creating it on
 * getCollection whether the collection doesn't exist yet.
 */
DB.prototype.getCollection = function() {
    let collection = originalGetCollection.apply(this, arguments);

    if (TestData.implicitlyShardOnCreateCollectionOnly) {
        return collection;
    }

    if (ShardingOverrideCommon.nssCanBeTrackedByShardingCatalog(collection.getFullName())) {
        return collection
    }

    if (!FixtureHelpers.isMongos(this)) {
        return collection;
    }

    const collectionsList = new DBCommandCursor(db, this.runCommand({
                                'listCollections': 1,
                                nameOnly: true,
                                filter: {name: collection.getName()}
                            })).toArray();

    if (collectionsList.length !== 0) {
        // Collection already exists
        return collection;
    }

    // Create the collection if it doesn't exist yet.
    // Ignoring NamespaceExists error in case the previous `listCollections` did a stale read on a
    // secondary.
    assert.commandWorkedOrFailedWithCode(
        ShardingOverrideCommon.createUnsplittableCollectionOnRandomShard(
            {db: this, collName: collection.getName()}),
        [ErrorCodes.NamespaceExists, ErrorCodes.AlreadyInitialized]);

    return collection;
};

/**
 * Override insert to recreate an unsplittable collection when it has been dropped
 */
DBCollection.prototype.insert = function() {
    const db = this.getDB();

    if (!FixtureHelpers.isMongos(db)) {
        return originalInsert.apply(this, arguments);
    }

    if (TestData.implicitlyShardOnCreateCollectionOnly) {
        return originalInsert.apply(this, arguments);
    }

    if (ShardingOverrideCommon.nssCanBeTrackedByShardingCatalog(this.getFullName())) {
        return originalInsert.apply(this, arguments);
    }

    const collName = this.getName();
    const collectionsList =
        new DBCommandCursor(
            db, db.runCommand({'listCollections': 1, nameOnly: true, filter: {name: collName}}))
            .toArray();

    if (collectionsList.length !== 0) {
        // Collection already exists
        return originalInsert.apply(this, arguments);
    }

    // Create the unsplittable collection.
    assert.commandWorkedOrFailedWithCode(
        ShardingOverrideCommon.createUnsplittableCollectionOnRandomShard(
            {db: db, collName: collName}),
        [ErrorCodes.NamespaceExists, ErrorCodes.AlreadyInitialized]);

    return originalInsert.apply(this, arguments);
};

/**
 * Override create command to create unsplittable collection on a random shard.
 */
DB.prototype.runCommand = function(obj, extra, queryOptions) {
    const mergedObj = this._mergeCommandOptions(obj, extra);
    const cmdName = Object.keys(mergedObj)[0];
    if (cmdName !== "create" || !FixtureHelpers.isMongos(this)) {
        return originalRunCommand.apply(this, [obj, extra, queryOptions]);
    }

    // Call original create method if the given options are not supported by
    // createUnsplittableCollection.
    let unsupportedOption = false;

    let cmdOptions = Object.merge({}, mergedObj);
    let nss = cmdOptions['create'];
    if (ShardingOverrideCommon.nssCanBeTrackedByShardingCatalog(nss)) {
        unsupportedOption = true
    }
    delete cmdOptions['create'];

    const optNames = Object.keys(cmdOptions);
    optNames.forEach((optName) => {
        if (kUnsupportedOptions.has(optName)) {
            unsupportedOption = true;
        }
    })

    if (unsupportedOption) {
        return originalRunCommand.apply(this, [obj, extra, queryOptions]);
    }

    let res = ShardingOverrideCommon.createUnsplittableCollectionOnRandomShard(
        {db: this, collName: nss, opts: cmdOptions});

    if (!res.ok && res.code === ErrorCodes.AlreadyInitialized) {
        // This can happen if we created the collection on getCollection and then chose a different
        // data shard when we created it above. However, we want to ensure that this is what
        // happened and the AlreadyInitialized error wasn't for some other reason so we issue the
        // command without specifying a dataShard.
        delete cmdOptions.dataShard;
        var cmd = {create: nss};
        Object.extend(cmd, cmdOptions);
        res = originalRunCommand.apply(this, [cmd]);
    }
    return res;
};

OverrideHelpers.prependOverrideInParallelShell(
    "jstests/libs/override_methods/implicitly_create_unsplittable_collections_on_random_shards.js");
