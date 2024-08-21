/**
 * A helper class to execute different kinds of test scenarios for $convert.
 */
class ConvertTest {
    constructor({coll, requiresFCV80, requiresFCV81}) {
        this.coll = coll;
        this.requiresFCV80 = requiresFCV80;
        this.requiresFCV81 = requiresFCV81;
    }

    populateCollection(docs) {
        this.coll.drop();
        const bulk = this.coll.initializeOrderedBulkOp();
        docs.forEach(doc => bulk.insert(doc));
        assert.commandWorked(bulk.execute());
    }

    getFormatField() {
        // The "format" field is not supported in FCVs prior to 8.0. Hence the we must not use it in
        // the pipelines unless the workload is guaranteed to not run on older FCVs.
        return this.requiresFCV80 || this.requiresFCV81 ? {format: "$format"} : {};
    }

    getByteOrderField() {
        // The "byteOrder" field is not supported in FCVs prior to 8.1. Hence the we must not use it
        // in the pipelines unless the workload is guaranteed to not run on older FCVs.
        return this.requiresFCV81 ? {byteOrder: "$byteOrder"} : {};
    }

    runValidConversionTest({conversionTestDocs}) {
        this.populateCollection(conversionTestDocs);

        const coll = this.coll;
        const formatField = this.getFormatField();
        const byteOrderField = this.getByteOrderField();

        {
            // Test $convert on each document.
            const pipeline = [
                {
                    $project: {
                        output: {
                            $convert:
                                {to: "$target", input: "$input", ...formatField, ...byteOrderField}
                        },
                        target: {$ifNull: ["$target.type", "$target"]},
                        expected: "$expected"
                    }
                },
                {$addFields: {outputType: {$type: "$output"}}},
                {$sort: {_id: 1}}
            ];
            const aggResult = coll.aggregate(pipeline).toArray();
            assert.eq(aggResult.length, conversionTestDocs.length);

            aggResult.forEach(doc => {
                assert.eq(
                    doc.outputType, doc.target, "Conversion to incorrect type: _id = " + doc._id);
                assert.eq(doc.output, doc.expected, "Unexpected conversion: _id = " + doc._id);
            });
        }

        {
            // Test each conversion using the shorthand $toBool, $toString, etc. syntax.
            const toUUIDCase = {
                case: {$eq: ["$target", {type: "binData", subtype: 4}]},
                then: {$toUUID: "$input"},
            };

            // We may be converting from BinData to numeric, and the shorthand conversions always
            // uses little endian, so we only run the conversions which specified little endian.
            const toIntBinDataCase = {
                case: {$in: ["$target", ["int", {type: "int"}]]},
                then: {
                    $cond: {
                        if: {$eq: ["$byteOrder", "little"]},
                        then: {$toInt: "$input"},
                        else: {$convert: {to: "$target", input: "$input", ...byteOrderField}}
                    }
                },
            };

            const toLongBinDataCase = {
                case: {$in: ["$target", ["long", {type: "long"}]]},
                then: {
                    $cond: {
                        if: {$eq: ["$byteOrder", "little"]},
                        then: {$toLong: "$input"},
                        else: {$convert: {to: "$target", input: "$input", ...byteOrderField}}
                    }
                },
            };

            const toDoubleBinDataCase = {
                case: {$in: ["$target", ["double", {type: "double"}]]},
                then: {
                    $cond: {
                        if: {$eq: ["$byteOrder", "little"]},
                        then: {$toDouble: "$input"},
                        else: {$convert: {to: "$target", input: "$input", ...byteOrderField}}
                    }
                },
            };

            const pipeline = [
                {
                    $project: {
                        output: {
                            $switch: {
                                branches: [
                                    ...(this.requiresFCV81 ? [toDoubleBinDataCase] : []),
                                    {
                                        case: {$in: ["$target", ["double", {type: "double"}]]},
                                        then: {$toDouble: "$input"}
                                    },
                                    {
                                        case: {$in: ["$target", ["objectId", {type: "objectId"}]]},
                                        then: {$toObjectId: "$input"}
                                    },
                                    {
                                        case: {$in: ["$target", ["bool", {type: "bool"}]]},
                                        then: {$toBool: "$input"}
                                    },
                                    {
                                        case: {$in: ["$target", ["date", {type: "date"}]]},
                                        then: {$toDate: "$input"}
                                    },
                                    // $toInt and $toLong with BinData are not supported in FCVs
                                    // prior to v8.1.
                                    ...(this.requiresFCV81 ? [toIntBinDataCase] : []),
                                    ...(this.requiresFCV81 ? [toLongBinDataCase] : []),
                                    {
                                        case: {$in: ["$target", ["int", {type: "int"}]]},
                                        then: {$toInt: "$input"}
                                    },
                                    {
                                        case: {$in: ["$target", ["long", {type: "long"}]]},
                                        then: {$toLong: "$input"}
                                    },
                                    {
                                        case: {$in: ["$target", ["decimal", {type: "decimal"}]]},
                                        then: {$toDecimal: "$input"}
                                    },
                                    {
                                        case: {
                                            $and: [
                                                {$in: ["$target", ["string", {type: "string"}]]},
                                                // $toString uses the 'auto' format for
                                                // BinData-to-string conversions.
                                                {$in: ["$format", ["auto", "uuid"]]}
                                            ]
                                        },
                                        then: {$toString: "$input"},
                                    },
                                    // $toUUID is not supported in FCVs prior to v8.0.
                                    ...(this.requiresFCV80 ? [toUUIDCase] : []),
                                ],
                                default: {
                                    $convert: {
                                        to: "$target",
                                        input: "$input",
                                        ...formatField,
                                        ...byteOrderField
                                    }
                                }
                            }
                        },
                        target: {$ifNull: ["$target.type", "$target"]},
                        expected: "$expected"
                    }
                },
                {$addFields: {outputType: {$type: "$output"}}},
                {$sort: {_id: 1}}
            ];
            const aggResult = coll.aggregate(pipeline).toArray();
            assert.eq(aggResult.length, conversionTestDocs.length);

            aggResult.forEach(doc => {
                assert.eq(
                    doc.outputType, doc.target, "Conversion to incorrect type: _id = " + doc._id);
                assert.eq(doc.output, doc.expected, "Unexpected conversion: _id = " + doc._id);
            });
        }
    }

    runIllegalConversionTest({illegalConversionTestDocs}) {
        // Test a $convert expression with "onError" to make sure that error handling still allows
        // an error in the "input" expression to propagate.
        assert.throws(function() {
            coll.aggregate([{
                $project:
                    {output: {$convert: {to: "string", input: {$divide: [1, 0]}, onError: "ERROR"}}}
            }]);
        }, [], "Pipeline should have failed");

        this.populateCollection(illegalConversionTestDocs);

        const coll = this.coll;
        const formatField = this.getFormatField();
        const byteOrderField = this.getByteOrderField();

        // Test each document to ensure that the conversion throws an error.
        illegalConversionTestDocs.forEach(doc => {
            const pipeline = [
                {$match: {_id: doc._id}},
                {
                    $project: {
                        output: {
                            $convert:
                                {to: "$target", input: "$input", ...formatField, ...byteOrderField}
                        }
                    }
                }
            ];

            assert.throws(function() {
                const res = coll.aggregate(pipeline);
                print("should have failed result:", tojson(res.toArray()));
            }, [], "Conversion should have failed: _id = " + doc._id);
        });

        {
            // Test that each illegal conversion uses the 'onError' value.
            const pipeline = [
                {
                    $project: {
                        output: {
                            $convert: {
                                to: "$target",
                                input: "$input",
                                ...formatField,
                                ...byteOrderField,
                                onError: "ERROR"
                            }
                        }
                    }
                },
                {$sort: {_id: 1}}
            ];
            const aggResult = coll.aggregate(pipeline).toArray();
            assert.eq(aggResult.length, illegalConversionTestDocs.length);

            aggResult.forEach(doc => {
                assert.eq(doc.output, "ERROR", "Unexpected result: _id = " + doc._id);
            });
        }

        {
            // Test that, when onError is missing, the missing value propagates to the result.
            const pipeline = [
                {
                    $project: {
                        _id: false,
                        output: {
                            $convert: {
                                to: "$target",
                                input: "$input",
                                ...formatField,
                                ...byteOrderField,
                                onError: "$$REMOVE"
                            }
                        }
                    }
                },
                {$sort: {_id: 1}}
            ];
            const aggResult = coll.aggregate(pipeline).toArray();
            assert.eq(aggResult.length, illegalConversionTestDocs.length);

            aggResult.forEach(doc => {
                assert.eq(doc, {});
            });
        }
    }

    runNullConversionTest({nullTestDocs}) {
        this.populateCollection(nullTestDocs);

        const coll = this.coll;

        {
            // Test that all nullish inputs result in the 'onNull' output.
            const pipeline = [
                {$project: {output: {$convert: {to: "int", input: "$input", onNull: "NULL"}}}},
                {$sort: {_id: 1}}
            ];
            const aggResult = coll.aggregate(pipeline).toArray();
            assert.eq(aggResult.length, nullTestDocs.length);

            aggResult.forEach(doc => {
                assert.eq(doc.output, "NULL", "Unexpected result: _id = " + doc._id);
            });
        }

        {
            // Test that all nullish inputs result in the 'onNull' output _even_ if 'to' is nullish.
            const pipeline = [
                {$project: {output: {$convert: {to: null, input: "$input", onNull: "NULL"}}}},
                {$sort: {_id: 1}}
            ];
            const aggResult = coll.aggregate(pipeline).toArray();
            assert.eq(aggResult.length, nullTestDocs.length);

            aggResult.forEach(doc => {
                assert.eq(doc.output, "NULL", "Unexpected result: _id = " + doc._id);
            });
        }
    }

    runInvalidTargetTypeTest({invalidTargetTypeDocs}) {
        this.populateCollection(invalidTargetTypeDocs);

        const coll = this.coll;
        const formatField = this.getFormatField();
        const byteOrderField = this.getByteOrderField();

        // Test that $convert returns a parsing error for invalid 'to' arguments.
        invalidTargetTypeDocs.forEach(doc => {
            // A parsing error is expected even when 'onError' is specified.
            for (const onError of [{}, {onError: "NULL"}]) {
                const pipeline = [
                    {$match: {_id: doc._id}},
                    {
                        $project: {
                            output: {
                                $convert: {
                                    to: "$target",
                                    input: "$input",
                                    ...formatField,
                                    ...byteOrderField,
                                    ...onError
                                }
                            }
                        }
                    },
                ];

                const error = assert.throws(() => coll.aggregate(pipeline));
                assert.commandFailedWithCode(
                    error,
                    doc.expectedCode,
                    "Conversion should have failed with parsing error: _id = " + doc._id);
            }
        });
    }
}

/*
 * Runs different scenarios that test the $convert aggregation operator.
 * @param {coll} the collection to use for running the tests.
 * @param {requiresFCV80} whether the test is guaranteed to run on at least FCV 8.0.
 * @param {requiresFCV81} whether the test is guaranteed to run on at least FCV 8.1.
 * @param {conversionTestDocs} valid conversions and their expected results.
 * @param {illegalConversionTestDocs} unsupported but syntactically valid conversions that can be
 *     suppressed by specifying onError.
 * @param {nullTestDocs} conversions with null(ish) input.
 * @param {invalidTargetTypeDocs} conversions invalid target type.
 */
export function runConvertTests({
    coll,
    requiresFCV80 = false,
    requiresFCV81 = false,
    conversionTestDocs = [],
    illegalConversionTestDocs = [],
    nullTestDocs = [],
    invalidTargetTypeDocs = [],
}) {
    const testRunner = new ConvertTest({coll, requiresFCV80, requiresFCV81});

    if (conversionTestDocs.length) {
        testRunner.runValidConversionTest({conversionTestDocs});
    }

    if (illegalConversionTestDocs.length) {
        testRunner.runIllegalConversionTest({illegalConversionTestDocs});
    }

    if (nullTestDocs.length) {
        testRunner.runNullConversionTest({nullTestDocs});
    }

    if (invalidTargetTypeDocs.length) {
        testRunner.runInvalidTargetTypeTest({invalidTargetTypeDocs});
    }
}
