/*
 * @tags: [
 *  # movePrimary command is not allowed in clusters with a single shard.
 *  requires_2_or_more_shards,
 *  # movePrimary fails if there is a stepdown during data cloning phase.
 *  does_not_support_stepdowns,
 * ]
 */

import {getRandomShardName} from "jstests/libs/sharded_cluster_fixture_helpers.js";

const filterSystemColl = {
    name: {'$not': /^system\./}
};

function checkOptions(coll, expectedOptions) {
    assert.hasFields(coll.getMetadata().options,
                     Object.keys(expectedOptions),
                     `Missing expected option(s) for collection '${coll.getFullName()}'`);
}

function checkUUID(coll, expectedUUID) {
    assert.eq(
        expectedUUID, coll.getUUID(), `Incorrect uuid for collection '${coll.getFullName()}'`);
}

function checkIndexes(coll, expectedIndexes) {
    assert.sameMembers(expectedIndexes,
                       coll.getIndexes(),
                       `Unexpected indexes found for collection '${coll.getFullName()}'`);
}

function createCollection(coll, options, indexes, sharded) {
    assert.commandWorked(db.createCollection(coll.getName(), options));

    for (let i = 0; i < 3; i++) {
        assert.commandWorked(coll.insert({a: i}));
    }
    assert.eq(3, coll.countDocuments({}));

    assert.commandWorked(db.runCommand({createIndexes: coll.getName(), indexes: indexes}));

    if (sharded) {
        assert.commandWorked(db.adminCommand({shardCollection: coll.getFullName(), key: {_id: 1}}));
    }
}

function testMovePrimary(sharded) {
    // ----------------------------
    // Setup collections
    // ----------------------------
    db.dropDatabase();

    const c1_name = 'c1';
    const c2_name = 'c2';

    const c1_options = {validationLevel: "off"};
    const c2_options = {validator: {$jsonSchema: {required: ['a']}}};

    const c1_index_specs = [{key: {a: 1}, name: 'index1', expireAfterSeconds: 5000}];
    const c2_index_specs = [{key: {a: -1}, name: 'index2'}];

    const c1 = db[c1_name];
    const c2 = db[c2_name];

    createCollection(c1, c1_options, c1_index_specs, sharded);
    createCollection(c2, c2_options, c2_index_specs, sharded);

    const c1_uuid = c1.getUUID();
    const c2_uuid = c2.getUUID();

    const c1_indexes = c1.getIndexes();
    const c2_indexes = c2.getIndexes();

    assert.eq(
        3, c1.countDocuments({}), 'Unexpected number of document after c1 collection creation');
    assert.eq(
        3, c2.countDocuments({}), 'Unexpected number of document after c2 collection creation');

    {
        const colls = db.getCollectionInfos(filterSystemColl);
        assert.eq(2,
                  colls.length,
                  `Unexpected number of collections found before moving primary: ${tojson(colls)}`);
    }

    // ----------------------------
    // Change database primary shard
    // ----------------------------
    const fromShard = db.getDatabasePrimaryShardId();
    const toShard = getRandomShardName(db, /* exclude= */ fromShard);

    assert.commandWorked(db.adminCommand({movePrimary: db.getName(), to: toShard}));

    // ----------------------------
    // Check post conditions
    // ----------------------------
    {
        const colls = db.getCollectionInfos(filterSystemColl);
        assert.eq(2,
                  colls.length,
                  `Unexpected number of collections found after moving primary: ${tojson(colls)}`);
    }

    assert(c1.exists());
    assert(c2.exists());

    checkOptions(c1, c1_options);
    checkOptions(c2, c2_options);
    checkIndexes(c1, c1_indexes);
    checkIndexes(c2, c2_indexes);

    // assert number of documents didn't change
    assert.eq(3, c1.countDocuments({}));
    assert.eq(3, c2.countDocuments({}));

    if (sharded) {
        // For untracked collection the movePrimary command will change the UUID as part
        // of the operation.
        checkUUID(c1, c1_uuid);
        checkUUID(c2, c2_uuid);
    }
}

testMovePrimary(/* sharded= */ false);
testMovePrimary(/* sharded= */ true);
