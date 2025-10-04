/**
 * Tests document validation errors with sample validators. Note that it uses some JSON Schemas from
 * schemastore.org.
 *
 * @tags: [
 *   requires_fcv_51
 * ]
 */
import {assertDocumentValidationFailure} from "jstests/libs/doc_validation_utils.js";

const thirdPartyJSONSchemaDirectory = "src/third_party/schemastore.org/src/schemas/json/"; // schemastore.org schemas.
const testDB = db.getSiblingDB(jsTestName());
const coll = testDB.doc_validation_error;

// Executes a test case that creates a collection with a validator specified, attempts to insert a
// document that fails validation and then checks the produced document validation error.
function executeDocumentValidationTestCase(testCase) {
    // Drop the test database.
    assert.commandWorked(testDB.dropDatabase());

    // Get the schema of document validator.
    let validator;
    if (typeof testCase.validator === "string") {
        // Load a JSON Schema from a file in the third-party schema directory using
        // 'testCase.validator' as a name of a file.
        validator = {
            $jsonSchema: JSON.parse(cat(thirdPartyJSONSchemaDirectory + testCase.validator)),
        };
    } else {
        validator = testCase.validator;
    }

    // Create a collection with a validator specified.
    assert.commandWorked(testDB.createCollection(coll.getName(), {validator: validator}));

    // Attempt to insert a document.
    const result = coll.insert(testCase.inputDocument);

    // Verify that document validation failed and the document validation error matches the
    // expected.
    assertDocumentValidationFailure(result, coll);
    assert.docEq(testCase.expectedError, result.getWriteError().errInfo.details, `Test case ${testCase.name}`);
}

const testCases = [
    {
        name: "Node structure validation", // From https://jira.mongodb.org/browse/FREE-150292.
        validator: {
            $jsonSchema: {
                bsonType: "object",
                required: ["_id"],
                properties: {
                    parent: {bsonType: ["string", "null"], description: "must be a string"},
                    children: {
                        bsonType: ["array"],
                        items: {bsonType: ["string"]},
                        minItems: 0,
                        maxItems: 2,
                        description: "must be an array of string and max is 2",
                    },
                },
            },
        },
        inputDocument: {_id: "Languages", children: 2, parent: "Programming"},
        expectedError: {
            operatorName: "$jsonSchema",
            schemaRulesNotSatisfied: [
                {
                    operatorName: "properties",
                    propertiesNotSatisfied: [
                        {
                            propertyName: "children",
                            description: "must be an array of string and max is 2",
                            details: [
                                {
                                    operatorName: "bsonType",
                                    specifiedAs: {bsonType: ["array"]},
                                    reason: "type did not match",
                                    consideredValue: 2,
                                    consideredType: "double",
                                },
                            ],
                        },
                    ],
                },
            ],
        },
    },
    {
        name: "array of enums", // From SERVER-32705.
        validator: {
            $jsonSchema: {
                properties: {
                    values: {
                        bsonType: "array",
                        items: {
                            bsonType: "string",
                            enum: ["one", "two", "three"],
                        },
                        minItems: 1,
                        uniqueItems: true,
                    },
                },
            },
        },
        inputDocument: {values: ["two", "two"]},
        expectedError: {
            operatorName: "$jsonSchema",
            schemaRulesNotSatisfied: [
                {
                    operatorName: "properties",
                    propertiesNotSatisfied: [
                        {
                            propertyName: "values",
                            details: [
                                {
                                    operatorName: "uniqueItems",
                                    specifiedAs: {uniqueItems: true},
                                    reason: "found a duplicate item",
                                    consideredValue: ["two", "two"],
                                    duplicatedValue: "two",
                                },
                            ],
                        },
                    ],
                },
            ],
        },
    },
    {
        name: "Duplicate check in an array", // From SERVER-1068.
        validator: {$expr: {$eq: [{$size: "$a.b"}, {$size: {$setUnion: "$a.b"}}]}},
        inputDocument: {a: [{b: 1}, {b: 1}]},
        expectedError: {
            operatorName: "$expr",
            specifiedAs: {$expr: {$eq: [{$size: "$a.b"}, {$size: {$setUnion: "$a.b"}}]}},
            reason: "expression did not match",
            expressionResult: false,
        },
    },
    {
        name: "Students collection", // From https://docs.mongodb.com/manual/core/schema-validation/.
        validator: {
            $jsonSchema: {
                bsonType: "object",
                required: ["name", "year", "major", "address"],
                properties: {
                    name: {bsonType: "string", description: "must be a string and is required"},
                    year: {
                        bsonType: "int",
                        minimum: 2017,
                        maximum: 3017,
                        description: "must be an integer in [ 2017, 3017 ] and is required",
                    },
                    major: {
                        enum: ["Math", "English", "Computer Science", "History", null],
                        description: "can only be one of the enum values and is required",
                    },
                    gpa: {bsonType: ["double"], description: "must be a double if the field exists"},
                    address: {
                        bsonType: "object",
                        required: ["city"],
                        properties: {
                            street: {
                                bsonType: "string",
                                description: "must be a string if the field exists",
                            },
                            city: {
                                bsonType: "string",
                                description: "must be a string and is required",
                            },
                        },
                    },
                },
            },
        },
        inputDocument: {
            year: NumberInt("2020"),
            major: "Software Engineering",
            gpa: 5,
            address: {city: "NYC", street: NumberInt("5")},
        },
        expectedError: {
            operatorName: "$jsonSchema",
            schemaRulesNotSatisfied: [
                {
                    operatorName: "properties",
                    propertiesNotSatisfied: [
                        {
                            propertyName: "major",
                            description: "can only be one of the enum values and is required",
                            details: [
                                {
                                    operatorName: "enum",
                                    specifiedAs: {
                                        enum: ["Math", "English", "Computer Science", "History", null],
                                    },
                                    reason: "value was not found in enum",
                                    consideredValue: "Software Engineering",
                                },
                            ],
                        },
                        {
                            propertyName: "address",
                            details: [
                                {
                                    operatorName: "properties",
                                    propertiesNotSatisfied: [
                                        {
                                            propertyName: "street",
                                            description: "must be a string if the field exists",
                                            details: [
                                                {
                                                    operatorName: "bsonType",
                                                    specifiedAs: {bsonType: "string"},
                                                    reason: "type did not match",
                                                    consideredValue: 5,
                                                    consideredType: "int",
                                                },
                                            ],
                                        },
                                    ],
                                },
                            ],
                        },
                    ],
                },
                {
                    operatorName: "required",
                    specifiedAs: {required: ["name", "year", "major", "address"]},
                    missingProperties: ["name"],
                },
            ],
        },
    },
    {
        name: "Contacts", // From https://docs.mongodb.com/manual/core/schema-validation/.
        validator: {
            $or: [
                {phone: {$type: "string"}},
                {email: {$regex: /@mongodb\.com$/}},
                {status: {$in: ["Unknown", "Incomplete"]}},
            ],
        },
        inputDocument: {email: "info@mongodb.co"},
        expectedError: {
            operatorName: "$or",
            clausesNotSatisfied: [
                {
                    index: 0,
                    details: {
                        operatorName: "$type",
                        specifiedAs: {phone: {$type: "string"}},
                        reason: "field was missing",
                    },
                },
                {
                    index: 1,
                    details: {
                        operatorName: "$regex",
                        specifiedAs: {email: {$regex: /@mongodb\.com$/}},
                        reason: "regular expression did not match",
                        consideredValue: "info@mongodb.co",
                    },
                },
                {
                    index: 2,
                    details: {
                        operatorName: "$in",
                        specifiedAs: {status: {$in: ["Unknown", "Incomplete"]}},
                        reason: "field was missing",
                    },
                },
            ],
        },
    },
    {
        name: "Contacts 2", // From https://docs.mongodb.com/manual/core/schema-validation/.
        validator: {
            $jsonSchema: {
                bsonType: "object",
                required: ["phone"],
                properties: {
                    phone: {bsonType: "string", description: "must be a string and is required"},
                    email: {
                        bsonType: "string",
                        pattern: "@mongodb\.com$",
                        description: "must be a string and match the regular expression pattern",
                    },
                    status: {
                        enum: ["Unknown", "Incomplete"],
                        description: "can only be one of the enum values",
                    },
                },
            },
        },
        inputDocument: {email: "info@mongodb.com", status: "Complete"},
        expectedError: {
            operatorName: "$jsonSchema",
            schemaRulesNotSatisfied: [
                {
                    operatorName: "properties",
                    propertiesNotSatisfied: [
                        {
                            propertyName: "status",
                            description: "can only be one of the enum values",
                            details: [
                                {
                                    operatorName: "enum",
                                    specifiedAs: {enum: ["Unknown", "Incomplete"]},
                                    reason: "value was not found in enum",
                                    consideredValue: "Complete",
                                },
                            ],
                        },
                    ],
                },
                {
                    operatorName: "required",
                    specifiedAs: {required: ["phone"]},
                    missingProperties: ["phone"],
                },
            ],
        },
    },
    {
        name: "component.json",
        validator: "component.json", // From schemastore.org adapted by removing unsupported features.
        inputDocument: {
            private: 0,
            name: "Mongo",
            description: "DB",
            version: NumberInt("1"),
            keywords: [],
            main: "index.js",
            scripts: [1, 2],
            dependencies: NumberInt("2"),
        },
        expectedError: {
            operatorName: "$jsonSchema",
            title: "JSON schema for component.json files",
            schemaRulesNotSatisfied: [
                {
                    operatorName: "properties",
                    propertiesNotSatisfied: [
                        {
                            propertyName: "private",
                            description:
                                "A boolean specifying whether the component is private, defaulting to    false.",
                            details: [
                                {
                                    operatorName: "type",
                                    specifiedAs: {type: "boolean"},
                                    reason: "type did not match",
                                    consideredValue: 0,
                                    consideredType: "double",
                                },
                            ],
                        },
                        {
                            propertyName: "name",
                            description:
                                "A public component MUST have a 'name'. This is what will be passed to    require().",
                            details: [
                                {
                                    operatorName: "pattern",
                                    specifiedAs: {pattern: "^[0-9a-z-_]+$"},
                                    reason: "regular expression did not match",
                                    consideredValue: "Mongo",
                                },
                            ],
                        },
                        {
                            propertyName: "version",
                            description:
                                "The public component MUST include a version, allowing other scripts to depend on specific releases of the component.",
                            details: [
                                {
                                    operatorName: "type",
                                    specifiedAs: {type: "string"},
                                    reason: "type did not match",
                                    consideredValue: 1,
                                    consideredType: "int",
                                },
                            ],
                        },
                        {
                            propertyName: "scripts",
                            description:
                                "The    scripts    field explicitly specifies the scripts for this component. For public components, these must be regular JavaScript files. For private components, these should be regular Javascript files.",
                            details: [
                                {
                                    operatorName: "items",
                                    reason: "At least one item did not match the sub-schema",
                                    itemIndex: 0,
                                    details: [
                                        {
                                            operatorName: "type",
                                            specifiedAs: {type: "string"},
                                            reason: "type did not match",
                                            consideredValue: 1,
                                            consideredType: "double",
                                        },
                                    ],
                                },
                            ],
                        },
                        {
                            propertyName: "dependencies",
                            description: "Runtime dependencies.",
                            details: [
                                {
                                    operatorName: "type",
                                    specifiedAs: {type: "object"},
                                    reason: "type did not match",
                                    consideredValue: 2,
                                    consideredType: "int",
                                },
                            ],
                        },
                    ],
                },
            ],
        },
    },
    {
        name: "LLVM compilation database",
        validator: "compile-commands.json", // From schemastore.org adapted by removing unsupported
        // features.
        inputDocument: {
            commands: [
                {directory: "/etc", file: "hosts"},
                {directory: "/etc", arguments: [NumberInt("5")]},
            ],
        },
        expectedError: {
            operatorName: "$jsonSchema",
            title: "LLVM compilation database",
            description:
                "Describes a format for specifying how to replay single compilations independently of the build system",
            schemaRulesNotSatisfied: [
                {
                    operatorName: "properties",
                    propertiesNotSatisfied: [
                        {
                            propertyName: "commands",
                            details: [
                                {
                                    operatorName: "items",
                                    reason: "At least one item did not match the sub-schema",
                                    itemIndex: 0,
                                    details: [
                                        {
                                            operatorName: "anyOf",
                                            schemasNotSatisfied: [
                                                {
                                                    index: 0,
                                                    details: [
                                                        {
                                                            operatorName: "required",
                                                            specifiedAs: {required: ["directory", "file", "command"]},
                                                            missingProperties: ["command"],
                                                        },
                                                    ],
                                                },
                                                {
                                                    index: 1,
                                                    details: [
                                                        {
                                                            operatorName: "required",
                                                            specifiedAs: {required: ["directory", "file", "arguments"]},
                                                            missingProperties: ["arguments"],
                                                        },
                                                    ],
                                                },
                                            ],
                                        },
                                    ],
                                },
                            ],
                        },
                    ],
                },
            ],
        },
    },
    {
        name: "adonisrc.schema.json",
        validator: "adonisrc.schema.json", // From schemastore.org.
        inputDocument: {
            preloads: [
                "PREL1",
                {file: "file1", environment: ["web", "console"]},
                {file: "file1", environment: ["web", "console"]},
                {file: "file2", dir: true},
            ],
            providers: ["A", "B", "C", "B"],
        },
        expectedError: {
            operatorName: "$jsonSchema",
            schemaRulesNotSatisfied: [
                {
                    operatorName: "properties",
                    propertiesNotSatisfied: [
                        {
                            propertyName: "preloads",
                            details: [
                                {
                                    operatorName: "uniqueItems",
                                    specifiedAs: {uniqueItems: true},
                                    reason: "found a duplicate item",
                                    consideredValue: [
                                        "PREL1",
                                        {file: "file1", environment: ["web", "console"]},
                                        {file: "file1", environment: ["web", "console"]},
                                        {file: "file2", dir: true},
                                    ],
                                    duplicatedValue: {file: "file1", environment: ["web", "console"]},
                                },
                                {
                                    operatorName: "items",
                                    reason: "At least one item did not match the sub-schema",
                                    itemIndex: 3,
                                    details: [
                                        {
                                            operatorName: "oneOf",
                                            schemasNotSatisfied: [
                                                {
                                                    index: 0,
                                                    details: [
                                                        {
                                                            operatorName: "type",
                                                            specifiedAs: {type: "string"},
                                                            reason: "type did not match",
                                                            consideredValue: {file: "file2", dir: true},
                                                            consideredType: "object",
                                                        },
                                                    ],
                                                },
                                                {
                                                    index: 1,
                                                    details: [
                                                        {
                                                            operatorName: "additionalProperties",
                                                            specifiedAs: {additionalProperties: false},
                                                            additionalProperties: ["dir"],
                                                        },
                                                    ],
                                                },
                                            ],
                                        },
                                    ],
                                },
                            ],
                        },
                        {
                            propertyName: "providers",
                            details: [
                                {
                                    operatorName: "uniqueItems",
                                    specifiedAs: {uniqueItems: true},
                                    reason: "found a duplicate item",
                                    consideredValue: ["A", "B", "C", "B"],
                                    duplicatedValue: "B",
                                },
                            ],
                        },
                    ],
                },
                {
                    operatorName: "required",
                    specifiedAs: {required: ["typescript", "providers"]},
                    missingProperties: ["typescript"],
                },
            ],
        },
    },
];
testCases.forEach(executeDocumentValidationTestCase);
