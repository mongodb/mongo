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
import {isFCVgte} from "jstests/libs/feature_compatibility_version.js";
import {isEndOfTransactionUsable} from "jstests/libs/query/change_stream_util.js";
import {QuerySettingsUtils} from "jstests/libs/query/query_settings_utils.js";

const coll = assertDropAndRecreateCollection(db, jsTestName());
const qsutils = new QuerySettingsUtils(db, coll.getName());

// Expected query hash for a change stream without any options
const kQueryHashForOptionLessStream =
    "ABD13DBF8CFAE39BF22941780FFCF8BB111582FB30C6CDC832933EA0E29F1C3D";

// An additional '$match' stage appended after the '$changeStream' stage. Adding stages to a change
// stream pipeline changes its representative query shape and thus its query shape hash.
const kExtraMatchStage = {$match: {x: 1}};

// Verifies that the query shape hash of 'query' is identical to 'queryShapeHash', and that the query
// shape hash of 'query' with an additional '$match' stage appended to its pipeline is identical to
// 'queryShapeHashWithExtraMatch'.
function checkQueryShapeAndHash(db, query, queryShapeHash, queryShapeHashWithExtraMatch) {
    verifyQueryShapeAndHash(query, queryShapeHash);
    verifyQueryShapeAndHash(
        {...query, pipeline: [...query.pipeline, kExtraMatchStage]},
        queryShapeHashWithExtraMatch,
    );
}

// Verifies that the querySettings for 'query' can be applied successfully, and that the query shape
// hash of the query is identical to 'queryShapeHash'. Also verifies that the representative query
// shape of the query is identical to 'query'.
function verifyQueryShapeAndHash(query, queryShapeHash) {
    qsutils.withQuerySettings(query, {reject: true}, () => {
        const hashFromSettings = qsutils.getQueryShapeHashFromQuerySettings(query);
        assert.eq(
            queryShapeHash,
            hashFromSettings,
            `Expected query shape (hash) for input query ${tojsononeline(query)}: (${
                queryShapeHash
            }), got ${hashFromSettings}. Full configuration: ${tojson(
                qsutils.getQuerySettings({showDebugQueryShape: true}),
            )}`,
        );
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
checkQueryShapeAndHash(
    db,
    {aggregate: 1, pipeline: [{$changeStream: {}}], $db: db.getName()},
    kQueryHashForOptionLessStream,
    "A6A2F2E3CB4A19B91D9577ED2C8AB56E92B1FD47A4A405143FC6A508264AC2B6",
);

// Check shape and hash for pipelines with 'showSystemEvents' flag.
// Using this flag changes the query shape and hash, regardless of whether the flag is set to the
// implicit default value (false) or not.
checkQueryShapeAndHash(
    db,
    {aggregate: 1, pipeline: [{$changeStream: {showSystemEvents: true}}], $db: db.getName()},
    "989509505CA0513EF576E1DA3C3D1319D8BCB6EF3A91E436C9A08FEA39AE428A",
    "EF11A530A393FFC1799C7A19E8992FCE2199838D9320C65474EDF9AA1950F7B4",
);

checkQueryShapeAndHash(
    db,
    {aggregate: 1, pipeline: [{$changeStream: {showSystemEvents: false}}], $db: db.getName()},
    "06F4F2FAD655A12343302BCCFD2702EEEA00A6D797D5EA0B1D89FC9396D20ADB",
    "5001D48395C90836307185EAB0E54F8489D9F4FFDE6ABD3AAEFA27DD8D1D64B0",
);

// Check shape and hash for pipelines with 'showExpandedEvents' flag.
// Using this flag changes the query shape and hash, regardless of whether the flag is set to the
// implicit default value (false) or not.
checkQueryShapeAndHash(
    db,
    {aggregate: 1, pipeline: [{$changeStream: {showExpandedEvents: true}}], $db: db.getName()},
    "8F93080A29997E20EE5C90B1DDF681DB8D0F3419531C00C9DE4843D80C7FEC3E",
    "95C173EFA57217FC8B75AD27C9F32EC0DA5580015F1AE99A4C05DC006EEC43AF",
);

checkQueryShapeAndHash(
    db,
    {aggregate: 1, pipeline: [{$changeStream: {showExpandedEvents: false}}], $db: db.getName()},
    "29524DD77A583945DFBDAC1909346D6565B2BEF5A607F6D9AD040982C12DEFCA",
    "458A10042DC51A46A3BE64BC153CCAC927EE4F8D761139ABAD0170065786EC37",
);

//
// Whole-cluster change streams.
//

// The following requires that query shapes are computed for whole-cluster change streams. This is
// the case only since 9.1.
if (isFCVgte(db, "9.1")) {
    // A whole-cluster change stream runs on the internal 'admin' database (admin.$cmd.aggregate) but
    // is a legitimate, user-facing query shape, so it gets a query shape and hash like any other.
    checkQueryShapeAndHash(
        db,
        {aggregate: 1, pipeline: [{$changeStream: {allChangesForCluster: true}}], $db: "admin"},
        "D40CE08EA9A7466FEE101997620FB83B46AEC58B1E92F16BD279A0CA3742FCBA",
        "0714E379C092CA2D1415402EE20F45C386D6525D9B48477EECAC5C3802629FE2",
    );

    // Check shape and hash for whole-cluster pipelines with the 'showSystemEvents' flag.
    checkQueryShapeAndHash(
        db,
        {
            aggregate: 1,
            pipeline: [{$changeStream: {allChangesForCluster: true, showSystemEvents: true}}],
            $db: "admin",
        },
        "D4C3BEA22A10E6135CFF04B2BCDB118EABA38E39B8106747139DED4D23F2A2E2",
        "7B5129B76DA162D06468FE4E96D172E4DC61F4D6DD2D20F832F4FCB28A3168D6",
    );

    checkQueryShapeAndHash(
        db,
        {
            aggregate: 1,
            pipeline: [{$changeStream: {allChangesForCluster: true, showSystemEvents: false}}],
            $db: "admin",
        },
        "A82B6EDF86E964522949C59451455A580736E6E76D288958556BE19ED3CE921B",
        "026B374BAFE326108709C6CA4785BAF7BF49BB3A5B18CC395173F7DD095BC2D4",
    );

    // Check shape and hash for whole-cluster pipelines with the 'showExpandedEvents' flag.
    checkQueryShapeAndHash(
        db,
        {
            aggregate: 1,
            pipeline: [{$changeStream: {allChangesForCluster: true, showExpandedEvents: true}}],
            $db: "admin",
        },
        "9F820A071BE4BFAEC1B0090CEF751A15821C91F23A53E32806116D5CDB9D82C8",
        "057F67265A54D51CCDC2391544037092CC537DA73622375AEECF633D1CC3A931",
    );

    checkQueryShapeAndHash(
        db,
        {
            aggregate: 1,
            pipeline: [{$changeStream: {allChangesForCluster: true, showExpandedEvents: false}}],
            $db: "admin",
        },
        "E4B847FB6DE58149DF021D136DA143EBE8F5160337DE3CC3F8FEBABD4BB1C70B",
        "B17CDC89EFAB63E7EA05F64C4128147F7879FBBFC8BDF49DB367645B3BF8D63D",
    );
}

//
// Collection-level change streams.
//

// Check shape and hash for a basic change stream pipeline.
checkQueryShapeAndHash(
    db,
    {aggregate: coll.getName(), pipeline: [{$changeStream: {}}], $db: db.getName()},
    "FF65C1F81BEB9C930D7C2A94661E1106F77C59809E0B1D7FB2AC3C6232D03A4C",
    "8D79D33E4BD1BC6412A711326F157977A8BC2DBEA1C7730976E0DB5B8C28DA4D",
);

// Check shape and hash for pipelines with 'showSystemEvents' flag.
// Using this flag changes the query shape and hash, regardless of whether the flag is set to the
// implicit default value (false) or not.
checkQueryShapeAndHash(
    db,
    {
        aggregate: coll.getName(),
        pipeline: [{$changeStream: {showSystemEvents: true}}],
        $db: db.getName(),
    },
    "DBE479E988FF18FA2DD5B2188A97A797BF1A6BD9571DD3CF3CEFAE4BDEF4E1D3",
    "22B47283D170B7FB85D3F3D5BEA16A6A6C1EC36AFA9173A1D73E8EB199BACD4E",
);

checkQueryShapeAndHash(
    db,
    {
        aggregate: coll.getName(),
        pipeline: [{$changeStream: {showSystemEvents: false}}],
        $db: db.getName(),
    },
    "AFD4BD81AC43C813818444EA835F951E1112E18D513F0BA255A867764A9ED48E",
    "C4CED2343789FBAB808FA5AB5E6FAA4835AF0E1B3EB2AAF2F7000C56260154C3",
);

// Check shape and hash for pipelines with 'showExpandedEvents' flag.
// Using this flag changes the query shape and hash, regardless of whether the flag is set to
// the implicit default value (false) or not.
checkQueryShapeAndHash(
    db,
    {
        aggregate: coll.getName(),
        pipeline: [{$changeStream: {showExpandedEvents: true}}],
        $db: db.getName(),
    },
    "97744EC94700017030246635FDFD4481C177EF25D2CFE7335ABD7E570BDE273E",
    "14949385B0473C47C12E62B8CED2C3113DBF8FBD570885E34842C7592AC05515",
);

checkQueryShapeAndHash(
    db,
    {
        aggregate: coll.getName(),
        pipeline: [{$changeStream: {showExpandedEvents: false}}],
        $db: db.getName(),
    },
    "8BFCDD5DA40E82A947514CD30503795B72B3B61DD2F5455133F1D7120F9394B2",
    "069C7D235727DB53218C65AA1B530A8B2F7B08474DCFB14C9B595EA73EE83DB0",
);

// Check shape and hash when setting the change stream reader version.
checkQueryShapeAndHash(
    db,
    {aggregate: 1, pipeline: [{$changeStream: {version: "v1"}}], $db: db.getName()},
    kQueryHashForOptionLessStream,
    "A6A2F2E3CB4A19B91D9577ED2C8AB56E92B1FD47A4A405143FC6A508264AC2B6",
);

checkQueryShapeAndHash(
    db,
    {aggregate: 1, pipeline: [{$changeStream: {version: "v2"}}], $db: db.getName()},
    kQueryHashForOptionLessStream,
    "A6A2F2E3CB4A19B91D9577ED2C8AB56E92B1FD47A4A405143FC6A508264AC2B6",
);

// Check shape and hash when setting the 'supportedEvents' field.
checkQueryShapeAndHash(
    db,
    {aggregate: 1, pipeline: [{$changeStream: {supportedEvents: ["foo"]}}], $db: db.getName()},
    kQueryHashForOptionLessStream,
    "A6A2F2E3CB4A19B91D9577ED2C8AB56E92B1FD47A4A405143FC6A508264AC2B6",
);

// Check shape and hash for a change stream pipeline using the 'ignoreRemovedShards' flag.
checkQueryShapeAndHash(
    db,
    {aggregate: 1, pipeline: [{$changeStream: {ignoreRemovedShards: true}}], $db: db.getName()},
    "796AFF268E3D2A16D9FFCD9F5929CB6C78D704560AC6F19A2BE40DC2B688200D",
    "5D1CECF3A9A9BDED874DFD128669F8A59C105F1249532F9BF3BFD5B35831E8E7",
);

if (isEndOfTransactionUsable(db)) {
    // Setting 'showEndOfTransaction' to 'off' is equivalent to not setting it at all: the option
    // is omitted from the serialized spec and from the query shape, so both produce the same query
    // shape and query shape hash as the basic change stream pipeline above.
    checkQueryShapeAndHash(
        db,
        {
            aggregate: 1,
            pipeline: [{$changeStream: {showEndOfTransaction: "off"}}],
            $db: db.getName(),
        },
        kQueryHashForOptionLessStream,
        "A6A2F2E3CB4A19B91D9577ED2C8AB56E92B1FD47A4A405143FC6A508264AC2B6",
    );

    checkQueryShapeAndHash(
        db,
        {
            aggregate: coll.getName(),
            pipeline: [{$changeStream: {showEndOfTransaction: "off"}}],
            $db: db.getName(),
        },
        "FF65C1F81BEB9C930D7C2A94661E1106F77C59809E0B1D7FB2AC3C6232D03A4C",
        "8D79D33E4BD1BC6412A711326F157977A8BC2DBEA1C7730976E0DB5B8C28DA4D",
    );

    // Setting 'showEndOfTransaction' to a value other than 'off' serializes the option and
    // therefore changes the query shape and query shape hash.
    checkQueryShapeAndHash(
        db,
        {
            aggregate: 1,
            pipeline: [{$changeStream: {showEndOfTransaction: "whenAvailable"}}],
            $db: db.getName(),
        },
        "37478EF31F7D7C5DBCC740A56C01A31400F5FEDCB34A94BE033B7DC016227C99",
        "FC813972C179B0CF555BFE19841BC8BDCCD7E023613F7495C11446580562DB7D",
    );

    checkQueryShapeAndHash(
        db,
        {
            aggregate: coll.getName(),
            pipeline: [{$changeStream: {showEndOfTransaction: "whenAvailable"}}],
            $db: db.getName(),
        },
        "0DBA3272557B116DC1AF298F2F7D3EF12A8A8D2F6CC1EAEAA307F89FDF0390FF",
        "21B1B96351141C5054FDA0CF4CC48919D647D7E7D98EC849CE8802A6F179F1D7",
    );

    checkQueryShapeAndHash(
        db,
        {
            aggregate: 1,
            pipeline: [{$changeStream: {showEndOfTransaction: "required"}}],
            $db: db.getName(),
        },
        "946A421D4FE4189DC4C47CD95D3E702BA86D7931BCB8EA4A3F2B61BCF2D30E77",
        "DA4EA7FBC648D6579A0D9734D0F6DD8D4B50BA526F0793D78CCDFF486AF98280",
    );

    checkQueryShapeAndHash(
        db,
        {
            aggregate: coll.getName(),
            pipeline: [{$changeStream: {showEndOfTransaction: "required"}}],
            $db: db.getName(),
        },
        "598EC3C06D740A3F259BCC284188AB86F8FA91DD194ACD27E89F41C7939F6484",
        "55E0DF08F9E93FB6F318FBC88E52DB377321D87DE1493B510E0117632AC9947C",
    );
}
