// Tests representative query shapes and hashes of change stream pipelines.
// @tags: [
//   requires_fcv80,
//   uses_change_streams,
//   change_stream_does_not_expect_txns,
//   directly_against_shardsvrs_incompatible,
//   does_not_support_stepdowns,
//   assumes_against_mongod_not_mongos,
// ]
//
import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {QuerySettingsUtils} from "jstests/libs/query/query_settings_utils.js";

const coll = assertDropAndRecreateCollection(db, jsTestName());
const qsutils = new QuerySettingsUtils(db, coll.getName());

// Verifies that the querySettings for 'query' can be applied successfully, and that the query shape
// hash of the query is identical to 'queryShapeHash'. Also verifies that the representative query
// shape of the query is identical to 'query'.
function checkQueryShapeAndHash(db, query, queryShapeHash) {
    qsutils.withQuerySettings(query, {reject: true}, () => {
        const hashFromSettings = qsutils.getQueryShapeHashFromQuerySettings(query);
        assert.eq(queryShapeHash,
                  hashFromSettings,
                  `Expected query shape (hash) for input query ${tojsononeline(query)}: (${
                      queryShapeHash}), got ${hashFromSettings}. Full configuration: ${
                      tojson(qsutils.getQuerySettings({showDebugQueryShape: true}))}`);
    });
}

// A general caveat regarding query shapes and query shape hashes:
// Many fields in "document_source_change_stream.idl" are optional fields with default values.
// Those fields will only be serialized and thus affect the representative query shape and hash if
// they are explicitly set to a value. If not explicitly set, these fields will not be serialized
// and not be part of the representative query shape nor hash. This is great for keeping
// downwards-compatibility of query shapes and query shape hashes when adding new fields. As a
// downside, this means that for semantically equivalent queries we can get different query shapes
// and hashes. The following semantically equivalent queries serve as an example for this, but the
// same is true for all optional fields:
//   {$changeStream: {}}                         => representative query: {$changeStream: {}}
//   {$changeStream: {showSystemEvents: false}}  => representative
//   query: {$changeStream: {showSystemEvents: false}}

//
// Database-level change streams.
//

// Check shape and hash for a basic change stream pipeline.
checkQueryShapeAndHash(db,
                       {aggregate: 1, pipeline: [{$changeStream: {}}], $db: db.getName()},
                       "ABD13DBF8CFAE39BF22941780FFCF8BB111582FB30C6CDC832933EA0E29F1C3D");

// Check shape and hash for pipelines with 'showSystemEvents' flag.
// Using this flag changes the query shape and hash, regardless of whether the flag is set to the
// implicit default value (false) or not.
checkQueryShapeAndHash(
    db,
    {aggregate: 1, pipeline: [{$changeStream: {showSystemEvents: true}}], $db: db.getName()},
    "989509505CA0513EF576E1DA3C3D1319D8BCB6EF3A91E436C9A08FEA39AE428A");

checkQueryShapeAndHash(
    db,
    {aggregate: 1, pipeline: [{$changeStream: {showSystemEvents: false}}], $db: db.getName()},
    "06F4F2FAD655A12343302BCCFD2702EEEA00A6D797D5EA0B1D89FC9396D20ADB");

// Check shape and hash for pipelines with 'showExpandedEvents' flag.
// Using this flag changes the query shape and hash, regardless of whether the flag is set to the
// implicit default value (false) or not.
checkQueryShapeAndHash(
    db,
    {aggregate: 1, pipeline: [{$changeStream: {showExpandedEvents: true}}], $db: db.getName()},
    "8F93080A29997E20EE5C90B1DDF681DB8D0F3419531C00C9DE4843D80C7FEC3E");

checkQueryShapeAndHash(
    db,
    {aggregate: 1, pipeline: [{$changeStream: {showExpandedEvents: false}}], $db: db.getName()},
    "29524DD77A583945DFBDAC1909346D6565B2BEF5A607F6D9AD040982C12DEFCA");

//
// Collection-level change streams.
//

// Check shape and hash for a basic change stream pipeline.
checkQueryShapeAndHash(
    db,
    {aggregate: coll.getName(), pipeline: [{$changeStream: {}}], $db: db.getName()},
    "FF65C1F81BEB9C930D7C2A94661E1106F77C59809E0B1D7FB2AC3C6232D03A4C");

// Check shape and hash for pipelines with 'showSystemEvents' flag.
// Using this flag changes the query shape and hash, regardless of whether the flag is set to the
// implicit default value (false) or not.
checkQueryShapeAndHash(db,
                       {
                           aggregate: coll.getName(),
                           pipeline: [{$changeStream: {showSystemEvents: true}}],
                           $db: db.getName()
                       },
                       "DBE479E988FF18FA2DD5B2188A97A797BF1A6BD9571DD3CF3CEFAE4BDEF4E1D3");

checkQueryShapeAndHash(db,
                       {
                           aggregate: coll.getName(),
                           pipeline: [{$changeStream: {showSystemEvents: false}}],
                           $db: db.getName()
                       },
                       "AFD4BD81AC43C813818444EA835F951E1112E18D513F0BA255A867764A9ED48E");

// Check shape and hash for pipelines with 'showExpandedEvents' flag.
// Using this flag changes the query shape and hash, regardless of whether the flag is set to
// the implicit default value (false) or not.
checkQueryShapeAndHash(db,
                       {
                           aggregate: coll.getName(),
                           pipeline: [{$changeStream: {showExpandedEvents: true}}],
                           $db: db.getName()
                       },
                       "97744EC94700017030246635FDFD4481C177EF25D2CFE7335ABD7E570BDE273E");

checkQueryShapeAndHash(db,
                       {
                           aggregate: coll.getName(),
                           pipeline: [{$changeStream: {showExpandedEvents: false}}],
                           $db: db.getName()
                       },
                       "8BFCDD5DA40E82A947514CD30503795B72B3B61DD2F5455133F1D7120F9394B2");

// The following tests rely on fields that were added to the $changeStreams stage in v8.2, so we
// only execute them if the FCV version is high enough.
const fcvDoc = db.adminCommand({getParameter: 1, featureCompatibilityVersion: 1});
if (MongoRunner.compareBinVersions(fcvDoc.featureCompatibilityVersion.version, "8.2") >= 0) {
    // Check shape and hash when setting the change stream reader version.
    checkQueryShapeAndHash(
        db,
        {aggregate: 1, pipeline: [{$changeStream: {version: "v1"}}], $db: db.getName()},
        "EC5DB9EA352364BD4061C6CBC605C887347ED0B30B9CE8D5B3BFFB6BE5F81AF5");

    checkQueryShapeAndHash(
        db,
        {aggregate: 1, pipeline: [{$changeStream: {version: "v2"}}], $db: db.getName()},
        "CA169C644BFED846C3782FF04DB2AA18CA0EBC141AF71654D77B245AD9C5B1F0");

    // Check shape and hash when setting the 'supportedEvents' field.
    checkQueryShapeAndHash(
        db,
        {aggregate: 1, pipeline: [{$changeStream: {supportedEvents: ["foo"]}}], $db: db.getName()},
        "B953C7FD7733C29EEEFB27EC963A2C9D123AB94311959A9B2749DD745BCBC838");

    // Check shape and hash for a change stream pipeline using the 'ignoreRemovedShards' flag.
    checkQueryShapeAndHash(
        db,
        {aggregate: 1, pipeline: [{$changeStream: {ignoreRemovedShards: true}}], $db: db.getName()},
        "796AFF268E3D2A16D9FFCD9F5929CB6C78D704560AC6F19A2BE40DC2B688200D");
}
