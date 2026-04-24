/**
 * Tests operations on existing suffixPreview and prefixPreview collections are blocked
 * after upgrade to a version that supports the "suffix" and "prefix" GA query types.
 */
import "jstests/multiVersion/libs/multi_rs.js";

import {EncryptedClient} from "jstests/fle2/libs/encrypted_client_util.js";
import {
    PrefixField,
    SuffixAndPrefixField,
    SuffixField,
    encStrNormalizedEqExpr,
} from "jstests/fle2/libs/qe_text_search_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const forcePreview = true;
const suffixPreviewField = new SuffixField(2, 5, true, false, 1, forcePreview);
const prefixPreviewField = new PrefixField(2, 5, false, true, 1, forcePreview);
const comboPreviewField = new SuffixAndPrefixField(2, 5, 2, 5, false, false, 1, forcePreview);
const suffixField = new SuffixField(2, 5, true, false, 1);
const prefixField = new PrefixField(2, 5, false, true, 1);
const comboField = new SuffixAndPrefixField(2, 5, 2, 5, false, false, 1);

function testPreviewDeprecationOnUpgrade(previewQueryTypeConfig, gaQueryTypeConfig, createError, crudError) {
    const rst = new ReplSetTest({
        nodes: 2,
        nodeOptions: {binVersion: "latest", setParameter: {featureFlagQEPrefixSuffixSearch: false}},
    });
    rst.startSet();
    rst.initiate();

    let client = new EncryptedClient(rst.getPrimary(), "dbTest");

    // GA types disabled:
    // - creating a new "suffixPreview" or "prefixPreview" collection succeeds.
    // - creating "prefix" or "suffix" collection fails
    const previewEFC = {
        encryptedFields: {
            "fields": [
                {
                    path: "field1",
                    bsonType: "string",
                    queries: previewQueryTypeConfig.createQueryTypeDescriptor(),
                },
            ],
        },
    };
    const gaEFC = {
        encryptedFields: {
            "fields": [
                {
                    path: "field1",
                    keyId: UUID("11d58b8a-0c6c-4d69-a0bd-70c6d9befae9"),
                    bsonType: "string",
                    queries: gaQueryTypeConfig.createQueryTypeDescriptor(),
                },
            ],
        },
    };
    assert.commandWorked(client.createEncryptionCollection("coll1", previewEFC));
    assert.commandFailedWithCode(client.getDB().createCollection("coll2", gaEFC), [11632900, 11632901]);

    assert.commandWorked(client.getDB().coll1.einsert({field1: "hello"}));

    // The upgrade enables GA types; but we should be able to start up with the preview collection
    rst.upgradeSet({binVersion: "latest", setParameter: {featureFlagQEPrefixSuffixSearch: true}});
    client = new EncryptedClient(rst.getPrimary(), "dbTest");

    // The upgrade enables GA types; we can no longer do CRUD ops on the preview collection
    assert.commandFailedWithCode(client.getDB().coll1.einsert({field1: "hello"}), crudError);

    const filter = encStrNormalizedEqExpr("field1", "hello");

    assert.commandFailedWithCode(client.getDB().coll1.eupdate(filter, {$set: {field1: "world"}}), crudError);

    assert.commandFailedWithCode(
        client.getDB().coll1.erunCommand({
            findAndModify: "coll1",
            query: filter,
            update: {$set: {field1: "world"}},
        }),
        crudError,
    );
    assert.commandFailedWithCode(
        client.getDB().coll1.erunCommand({delete: "coll1", deletes: [{q: filter, limit: 1}]}),
        crudError,
    );

    assert.commandFailedWithCode(client.getDB().erunCommand({find: "coll1", filter: filter}), crudError);

    // Finds without encrypted fields should still work
    assert.commandWorked(client.getDB().erunCommand({find: "coll1", filter: {}}));

    // The upgrade enables GA types; we can no longer create new preview collections
    assert.commandFailedWithCode(client.getDB().createCollection("coll2", previewEFC), createError);
    // we can now create GA collections
    assert.commandWorked(client.createEncryptionCollection("coll3", gaEFC));

    // We should be able to drop the preview collection
    client.getDB().coll1.drop();
    rst.stopSet();
}

testPreviewDeprecationOnUpgrade(suffixPreviewField, suffixField, 12341601, 12341603);
testPreviewDeprecationOnUpgrade(prefixPreviewField, prefixField, 12341600, 12341602);
testPreviewDeprecationOnUpgrade(comboPreviewField, comboField, 12341600, 12341602);
