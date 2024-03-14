/*
 * Test cases and common TS queries that use block processing.
 */
export function blockProcessingTestCases(
    timeFieldName, metaFieldName, datePrefix, dateUpperBound, dateLowerBound, sbeFullEnabled) {
    return [
        {
            name: "GroupByNull",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {$group: {_id: null}},
                {$project: {_id: 1}}
            ],
            usesBlockProcessing: sbeFullEnabled
        },
        {
            name: "Min_GroupByNull",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {$group: {_id: null, a: {$min: '$y'}}},
                {$project: {_id: 0, a: 1}}
            ],
            usesBlockProcessing: sbeFullEnabled
        },
        {
            name: "Min_GroupByNullAllPass",
            pipeline: [
                {$match: {[timeFieldName]: {$gt: dateLowerBound}}},
                {$group: {_id: null, a: {$min: '$y'}}},
                {$project: {_id: 0, a: 1}}
            ],
            usesBlockProcessing: sbeFullEnabled
        },
        {
            name: "MinWithId_GroupByNull",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {$group: {_id: null, a: {$min: '$y'}}}
            ],
            usesBlockProcessing: sbeFullEnabled
        },
        {
            name: "Max_GroupByNull",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {$group: {_id: null, a: {$max: '$y'}}},
                {$project: {_id: 0, a: 1}}
            ],
            usesBlockProcessing: sbeFullEnabled
        },
        {
            name: "Max_GroupByNullAllPass",
            pipeline: [
                {$match: {[timeFieldName]: {$gt: dateLowerBound}}},
                {$group: {_id: null, a: {$max: '$y'}}},
                {$project: {_id: 0, a: 1}}
            ],
            usesBlockProcessing: sbeFullEnabled
        },
        {
            name: "MaxWithId_GroupByNull",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {$group: {_id: null, a: {$max: '$y'}}}
            ],
            usesBlockProcessing: sbeFullEnabled
        },
        {
            name: "MaxMinusMin_GroupByNull",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {$group: {_id: null, a: {$min: '$y'}, b: {$max: '$y'}}},
                {$project: {_id: 0, a: {$subtract: ['$b', '$a']}}}
            ],
            usesBlockProcessing: sbeFullEnabled
        },
        {
            name: "MaxMinusMin_GroupByNullAllPass",
            pipeline: [
                {$match: {[timeFieldName]: {$gt: dateLowerBound}}},
                {$group: {_id: null, a: {$min: '$y'}, b: {$max: '$y'}}},
                {$project: {_id: 0, a: {$subtract: ['$b', '$a']}}}
            ],
            usesBlockProcessing: sbeFullEnabled
        },
        {
            name: "MaxMinusMinWithId_GroupByNull",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {$group: {_id: null, a: {$min: '$y'}, b: {$max: '$y'}}},
                {$project: {_id: 1, a: {$subtract: ['$b', '$a']}}}
            ],
            usesBlockProcessing: sbeFullEnabled
        },
        {
            name: "MinAndMaxWithId_GroupByNull",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {$group: {_id: null, a: {$min: '$y'}, b: {$max: '$y'}}}
            ],
            usesBlockProcessing: sbeFullEnabled
        },
        {
            name: "Min_GroupByX",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {$group: {_id: '$x', a: {$min: '$y'}}},
                {$project: {_id: 0, a: 1}}
            ],
            usesBlockProcessing: sbeFullEnabled
        },
        {
            name: "MinWithId_GroupByX",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {$group: {_id: '$x', a: {$min: '$y'}}}
            ],
            usesBlockProcessing: sbeFullEnabled
        },
        {
            name: "Max_GroupByX",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {$group: {_id: '$x', a: {$max: '$y'}}},
                {$project: {_id: 0, a: 1}}
            ],
            usesBlockProcessing: sbeFullEnabled
        },
        {
            name: "MaxWithId_GroupByX",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {$group: {_id: '$x', a: {$max: '$y'}}}
            ],
            usesBlockProcessing: sbeFullEnabled
        },
        {
            name: "MaxMinusMin_GroupByX",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {$group: {_id: '$x', a: {$min: '$y'}, b: {$max: '$y'}}},
                {$project: {_id: 0, a: {$subtract: ['$b', '$a']}}}
            ],
            usesBlockProcessing: sbeFullEnabled
        },
        {
            name: "MaxMinusMinWithId_GroupByX",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {$group: {_id: '$x', a: {$min: '$y'}, b: {$max: '$y'}}},
                {$project: {_id: 1, a: {$subtract: ['$b', '$a']}}}
            ],
            usesBlockProcessing: sbeFullEnabled
        },
        {
            name: "MinAndMaxWithId_GroupByX",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {$group: {_id: '$x', a: {$min: '$y'}, b: {$max: '$y'}}}
            ],
            usesBlockProcessing: sbeFullEnabled
        },
        {
            name: "MaxMinusMinWithId_GroupByDateTrunc",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {
                    $group: {
                        _id: {$dateTrunc: {date: "$time", unit: "hour"}},
                        a: {$min: '$y'},
                        b: {$max: '$y'}
                    }
                },
                {$project: {_id: 1, a: {$subtract: ['$b', '$a']}}}
            ],
            usesBlockProcessing: sbeFullEnabled
        },
        {
            name: "MinWithId_GroupByDateAdd",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {
                    $group: {
                        _id: {$dateAdd: {startDate: "$time", unit: "millisecond", amount: 100}},
                        a: {$min: '$y'}
                    }
                },
                {$project: {_id: 1, a: '$a'}}
            ],
            usesBlockProcessing: sbeFullEnabled
        },
        {
            name: "MinWithId_GroupByDateAddAndDateDiff",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {
                    $group: {
                        _id: {
                            $dateAdd: {
                                startDate: ISODate("2024-01-01T00:00:00"),
                                unit: "millisecond",
                                amount: {
                                    $dateDiff: {
                                        startDate: new Date(datePrefix),
                                        endDate: "$time",
                                        unit: "millisecond"
                                    }
                                }
                            }
                        },
                        a: {$min: '$y'}
                    }
                },
                {$project: {_id: 1, a: '$a'}}
            ],
            usesBlockProcessing: false
        },
        {
            name: "MinWithId_GroupByDateSubtract",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {
                    $group: {
                        _id:
                            {$dateSubtract: {startDate: "$time", unit: "millisecond", amount: 100}},
                        a: {$min: '$y'}
                    }
                },
                {$project: {_id: 1, a: '$a'}}
            ],
            usesBlockProcessing: sbeFullEnabled
        },
        {
            name: "MinWithId_GroupByDateSubtractAndDateDiff",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {
                    $group: {
                        _id: {
                            $dateSubtract: {
                                startDate: ISODate("2024-01-01T00:00:00"),
                                unit: "millisecond",
                                amount: {
                                    $dateDiff: {
                                        startDate: new Date(datePrefix),
                                        endDate: "$time",
                                        unit: "millisecond"
                                    }
                                }
                            }
                        },
                        a: {$min: '$y'}
                    }
                },
                {$project: {_id: 1, a: '$a'}}
            ],
            usesBlockProcessing: false
        },
        {
            name: "MaxAndMinOfDateDiffWithId_GroupByNull",
            pipeline: [
                {$match: {[timeFieldName]: {$lte: dateUpperBound}}},
                {
                    $group: {
                        _id: null,
                        a: {
                            $min: {
                                $dateDiff: {
                                    startDate: new Date(datePrefix),
                                    endDate: "$time",
                                    unit: "millisecond"
                                }
                            }
                        },
                        b: {
                            $max: {
                                $dateDiff: {
                                    startDate: new Date(datePrefix),
                                    endDate: "$time",
                                    unit: "millisecond"
                                }
                            }
                        },
                        c: {
                            $min: {
                                $dateDiff: {
                                    startDate: "$time",
                                    endDate: new Date(datePrefix),
                                    unit: "millisecond"
                                }
                            }
                        },
                        d: {
                            $max: {
                                $dateDiff: {
                                    startDate: "$time",
                                    endDate: new Date(datePrefix),
                                    unit: "millisecond"
                                }
                            }
                        },
                    }
                },
                {$project: {_id: 0, a: 1, b: 1, c: 1, d: 1}}
            ],
            usesBlockProcessing: sbeFullEnabled
        },
        {
            name: "MaxAndMinOfDateAddDateSubtractDateTruncWithId_GroupByNull",
            pipeline: [
                {$match: {[timeFieldName]: {$lte: dateUpperBound}}},
                {
                    $group: {
                        _id: null,
                        a: {
                            $min: {$dateAdd: {startDate: "$time", unit: "millisecond", amount: 100}}
                        },
                        b: {
                            $max: {
                                $dateSubtract:
                                    {startDate: "$time", unit: "millisecond", amount: 100}
                            }
                        },
                        c: {$max: {$dateTrunc: {date: "$time", unit: "second"}}},
                    }
                },
                {$project: {_id: 0, a: 1, b: 1, c: 1}}
            ],
            usesBlockProcessing: sbeFullEnabled
        },
        {
            name: "MaxMinusMinWithId_GroupByDateTruncAndDateAdd",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {
                    $group: {
                        _id: {
                            $dateTrunc: {
                                date: {
                                    $dateAdd: {startDate: "$time", unit: "millisecond", amount: 100}
                                },
                                unit: "hour"
                            }
                        },
                        a: {$min: '$y'},
                        b: {$max: '$y'}
                    }
                },
                {$project: {_id: 1, a: {$subtract: ['$b', '$a']}}}
            ],
            usesBlockProcessing: sbeFullEnabled
        },
        {
            name: "MaxMinusMinWithId_GroupByDateTruncAndDateSubtract",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {
                    $group: {
                        _id: {
                            $dateTrunc: {
                                date: {
                                    $dateSubtract:
                                        {startDate: "$time", unit: "millisecond", amount: 100}
                                },
                                unit: "hour"
                            }
                        },
                        a: {$min: '$y'},
                        b: {$max: '$y'}
                    }
                },
                {$project: {_id: 1, a: {$subtract: ['$b', '$a']}}}
            ],
            usesBlockProcessing: sbeFullEnabled
        },
        {
            name: "MinWithId_GroupByDateDiffAndDateAdd",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {
                    $group: {
                        _id: {
                            $dateDiff: {
                                startDate: new Date(datePrefix),
                                endDate: {
                                    $dateAdd: {startDate: "$time", unit: "millisecond", amount: 100}
                                },
                                unit: "millisecond"
                            }
                        },
                        a: {$min: '$y'},
                    }
                },
                {$project: {_id: 1, a: 1}}
            ],
            usesBlockProcessing: sbeFullEnabled
        },
        {
            name: "MinOfDateDiffWithId_GroupByNull_InvalidDate",
            pipeline: [
                {$match: {[timeFieldName]: {$lte: dateUpperBound}}},
                {
                    $group: {
                        _id: null,
                        a: {
                            $min: {
                                $dateDiff: {
                                    startDate: new Date(datePrefix),
                                    endDate: "$y",
                                    unit: "millisecond"
                                }
                            }
                        },
                    }
                },
                {$project: {_id: 0, a: 1}}
            ],
            expectedErrorCode: 7157922,
            usesBlockProcessing: sbeFullEnabled
        },
        {
            name: "MinOfDateAddWithId_GroupByNull_MissingAmount",
            pipeline: [
                {$match: {[timeFieldName]: {$lte: dateUpperBound}}},
                {
                    $group: {
                        _id: null,
                        a: {
                            $min: {
                                $dateAdd: {
                                    startDate: new Date(datePrefix),
                                    unit: "millisecond",
                                    amount: "$k"
                                }
                            }
                        },
                    }
                },
                {$project: {_id: 0, a: 1}}
            ],
            usesBlockProcessing: false
        },
        {
            name: "MaxPlusMinWithId_GroupByDateDiff",
            pipeline: [
                {$match: {[timeFieldName]: {$lte: dateUpperBound}}},
                {
                    $group: {
                        _id: {
                            $dateDiff: {
                                startDate: new Date(datePrefix),
                                endDate: "$time",
                                unit: "millisecond"
                            }
                        },
                        a: {$min: '$y'},
                        b: {$max: '$y'}
                    }
                },
                {$project: {_id: 1, a: {$add: ['$b', '$a']}}}
            ],
            usesBlockProcessing: sbeFullEnabled
        },
        {
            name: "MaxPlusMinWithId_GroupByFilteredComputedDateDiff",
            pipeline: [
                {$match: {[timeFieldName]: {$lte: new Date(datePrefix + 300)}}},
                {
                    $addFields: {
                        msDiff: {
                            $dateDiff: {
                                startDate: new Date(datePrefix),
                                endDate: "$time",
                                unit: "millisecond"
                            }
                        }
                    }
                },
                {$match: {msDiff: {$gte: 300}}},
                {$group: {_id: "$msDiff", a: {$min: '$y'}, b: {$max: '$y'}}},
                {$project: {_id: 1, a: {$add: ['$b', '$a']}}}
            ],
            usesBlockProcessing: sbeFullEnabled
        },
        {
            name: "Min_GroupByX_NoFilter",
            pipeline: [{$group: {_id: '$x', a: {$min: '$y'}}}, {$project: {_id: 0, a: 1}}],
            usesBlockProcessing: sbeFullEnabled
        },
        {
            name: "MinWithId_GroupByX_NoFilter",
            pipeline: [{$group: {_id: '$x', a: {$min: '$y'}}}],
            usesBlockProcessing: sbeFullEnabled
        },
        {
            name: "Max_GroupByX_NoFilter",
            pipeline: [{$group: {_id: '$x', a: {$max: '$y'}}}, {$project: {_id: 0, a: 1}}],
            usesBlockProcessing: sbeFullEnabled
        },
        {
            name: "MaxWithId_GroupByX_NoFilter",
            pipeline: [{$group: {_id: '$x', a: {$max: '$y'}}}],
            usesBlockProcessing: sbeFullEnabled
        },
        {
            name: "MaxMinusMin_GroupByX_NoFilter",
            pipeline: [
                {$group: {_id: '$x', a: {$min: '$y'}, b: {$max: '$y'}}},
                {$project: {_id: 0, a: {$subtract: ['$b', '$a']}}}
            ],
            usesBlockProcessing: sbeFullEnabled
        },
        {
            name: "MaxMinusMinWithId_GroupByX_NoFilter",
            pipeline: [
                {$group: {_id: '$x', a: {$min: '$y'}, b: {$max: '$y'}}},
                {$project: {_id: 1, a: {$subtract: ['$b', '$a']}}}
            ],
            usesBlockProcessing: sbeFullEnabled
        },
        {
            name: "MinAndMaxWithId_GroupByX_NoFilter",
            pipeline: [{$group: {_id: '$x', a: {$min: '$y'}, b: {$max: '$y'}}}],
            usesBlockProcessing: sbeFullEnabled
        },
        {
            name: "MaxMinusMinWithId_GroupByDateTrunc_NoFilter",
            pipeline: [
                {
                    $group: {
                        _id: {"$dateTrunc": {date: "$time", unit: "minute", binSize: 1}},
                        a: {$min: '$y'},
                        b: {$max: '$y'}
                    }
                },
                {$project: {_id: 1, a: {$subtract: ['$b', '$a']}}}
            ],
            usesBlockProcessing: sbeFullEnabled
        },
        {
            name: "MaxMinusMinWithId_GroupByDateTruncAndDateDiff_NoFilter",
            pipeline: [
                {
                    $group: {
                        _id: {
                            date: {$dateTrunc: {date: "$time", unit: "millisecond", binSize: 200}},
                            delta: {
                                $dateDiff: {
                                    startDate: new Date(datePrefix),
                                    endDate: "$time",
                                    unit: "millisecond"
                                }
                            }
                        },
                        a: {$min: '$y'},
                        b: {$max: '$y'}
                    }
                },
                {$project: {_id: 1, a: {$subtract: ['$b', '$a']}}}
            ],
            usesBlockProcessing: false
        },
        {
            name: "MaxMinusMinWithId_GroupByDateTruncAndMeta_NoFilter",
            pipeline: [
                {
                    $group: {
                        _id: {
                            date: {$dateTrunc: {date: "$time", unit: "minute", binSize: 1}},
                            symbol: "$measurement"
                        },
                        a: {$min: '$y'},
                        b: {$max: '$y'}
                    }
                },
                {$project: {_id: 1, a: {$subtract: ['$b', '$a']}}}
            ],
            usesBlockProcessing: false
        },
        {
            name: "MaxMinusMinWithId_GroupByMeta_NoFilter",
            pipeline: [
                {$group: {_id: "$measurement", a: {$min: '$y'}, b: {$max: '$y'}}},
                {$project: {_id: 1, a: {$subtract: ['$b', '$a']}}}
            ],
            usesBlockProcessing: false
        },
        {
            name: "Avg_GroupByX",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {$group: {_id: '$x', a: {$avg: '$y'}}},
                {$project: {_id: 0, a: 1}}
            ],
            usesBlockProcessing: false
        },
        {
            name: "Min_GroupByXAndY",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {$group: {_id: {x: '$x', y: '$y'}, a: {$min: '$z'}}},
                {$project: {_id: 0, a: 1}}
            ],
            usesBlockProcessing: false
        },
        {
            name: "Min_GroupByMetaSortKey",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {$group: {_id: {$meta: 'sortKey'}, a: {$min: '$y'}}},
                {$project: {_id: 0, a: 1}}
            ],
            usesBlockProcessing: false
        },
        {
            name: "MinOfMetaSortKey_GroupByX",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {$group: {_id: '$x', a: {$min: {$meta: 'sortKey'}}}},
                {$project: {_id: 0, a: 1}}
            ],
            usesBlockProcessing: false
        },
        {
            name: "GroupWithProjectedOutFieldInAccumulator",
            pipeline: [
                {$project: {_id: 0}},
                {$match: {[metaFieldName]: "foo"}},
                {$group: {_id: null, minY: {$min: "$y"}}},
            ],
            usesBlockProcessing: sbeFullEnabled
        },
        {
            name: "GroupWithProjectedOutFieldInGb",
            pipeline: [
                {$project: {_id: 0}},
                {$match: {[metaFieldName]: "foo"}},
                {$group: {_id: "$y", a: {$min: "$x"}}},
            ],
            usesBlockProcessing: sbeFullEnabled
        },
        {
            name: "GroupWithMixOfProjectedOutField",
            pipeline: [
                {$project: {_id: 0, x: 1 /* y not included */}},
                {$match: {[metaFieldName]: "foo"}},
                {$group: {_id: "$y", a: {$min: "$x"}}},
            ],
            usesBlockProcessing: false
        }
    ];
}
