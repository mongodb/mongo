// Verify valid and invalid scenarios for create encrypted collection

/**
 * @tags: [
 * assumes_unsharded_collection,
 * requires_fcv_70,
 * assumes_balancer_off,
 * ]
 */
let dbTest = db.getSiblingDB("create_encrypted_collection_db");

dbTest.dropDatabase();

function encryptedFields(overrides = {}) {
    const ef = {
        "fields": [
            {
                "path": "firstName",
                "keyId": UUID("11d58b8a-0c6c-4d69-a0bd-70c6d9befae9"),
                "bsonType": "string",
                "queries": {"queryType": "equality"},
            },
        ],
    };
    return Object.assign(ef, overrides);
}

const sampleEncryptedFields = encryptedFields();

assert.commandFailedWithCode(
    dbTest.createCollection("basic", {viewOn: "foo", encryptedFields: sampleEncryptedFields}),
    6346401,
    "Create with encryptedFields and viewOn passed",
);

assert.commandFailedWithCode(
    dbTest.createCollection("basic", {timeseries: {timeField: "time"}, encryptedFields: sampleEncryptedFields}),
    6346401,
    "Create with encryptedFields and timeseries passed",
);

assert.commandFailedWithCode(
    dbTest.createCollection("basic", {capped: true, size: 100000, encryptedFields: sampleEncryptedFields}),
    6367301,
    "Create with encryptedFields and capped passed",
);

assert.commandFailedWithCode(
    dbTest.createCollection("basic", {validationAction: "warn", encryptedFields: sampleEncryptedFields}),
    ErrorCodes.BadValue,
);

assert.commandFailedWithCode(
    dbTest.createCollection("basic", {encryptedFields: sampleEncryptedFields, validationLevel: "off"}),
    ErrorCodes.BadValue,
);

assert.commandFailedWithCode(
    dbTest.createCollection("basic", {encryptedFields: sampleEncryptedFields, validationLevel: "moderate"}),
    ErrorCodes.BadValue,
);

assert.commandWorked(dbTest.createCollection("basic", {encryptedFields: sampleEncryptedFields}));

const result = dbTest.getCollectionInfos({name: "basic"});
const ef = result[0].options.encryptedFields;
assert.eq(ef.escCollection, "enxcol_.basic.esc");
assert.eq(ef.ecocCollection, "enxcol_.basic.ecoc");

assert.commandWorked(
    dbTest.createCollection("basic_int64_cf", {
        encryptedFields: {
            "fields": [
                {
                    "path": "firstName",
                    "keyId": UUID("11d58b8a-0c6c-4d69-a0bd-70c6d9befae9"),
                    "bsonType": "string",
                    "queries": {"queryType": "equality", contention: NumberLong(123)},
                },
            ],
        },
    }),
);

assert.commandWorked(
    dbTest.createCollection("basic_int32_cf", {
        encryptedFields: {
            "fields": [
                {
                    "path": "firstName",
                    "keyId": UUID("11d58b8a-0c6c-4d69-a0bd-70c6d9befae9"),
                    "bsonType": "string",
                    "queries": {"queryType": "equality", contention: NumberInt(123)},
                },
            ],
        },
    }),
);

// createCollection with invalid state collection names
const stateCollNameInvalidFormatTests = [
    {escCollection: "legit.esc", ecocCollection: "enxcol_.legit.ecoc", code: 7406900},
    {escCollection: "enxcol_.legit.ecoc", ecocCollection: "enxcol_.legit.ecoc", code: 7406900},
    {escCollection: "enxcol_.legit.esc", ecocCollection: "legit.ecoc", code: 7406902},
    {escCollection: "enxcol_.legit.esc", ecocCollection: "enxcol_.legit.esc", code: 7406902},
    {escCollection: "enxcol_.invalid.esc", ecocCollection: "enxcol_.legit.ecoc", code: 13049800},
    {escCollection: "enxcol_.legit.esc", ecocCollection: "enxcol_.invalid.ecoc", code: 13049801},
];
for (let testCase of stateCollNameInvalidFormatTests) {
    let code = testCase.code;
    delete testCase.code;
    assert.commandFailedWithCode(
        dbTest.createCollection("legit", {
            encryptedFields: encryptedFields(testCase),
        }),
        code,
        `createCollection should reject state collection names ${tojson(testCase)}`,
    );
}
