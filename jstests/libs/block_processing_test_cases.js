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
            name: "GroupByNull_MinWithoutId",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {$group: {_id: null, a: {$min: '$y'}}},
                {$project: {_id: 0, a: 1}}
            ],
            usesBlockProcessing: sbeFullEnabled
        },
        {
            name: "GroupByNull_MinWithoutIdAllPass",
            pipeline: [
                {$match: {[timeFieldName]: {$gt: dateLowerBound}}},
                {$group: {_id: null, a: {$min: '$y'}}},
                {$project: {_id: 0, a: 1}}
            ],
            usesBlockProcessing: sbeFullEnabled
        },
        {
            name: "GroupByNull_Min",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {$group: {_id: null, a: {$min: '$y'}}}
            ],
            usesBlockProcessing: sbeFullEnabled
        },
        {
            name: "GroupByNull_MaxWithoutId",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {$group: {_id: null, a: {$max: '$y'}}},
                {$project: {_id: 0, a: 1}}
            ],
            usesBlockProcessing: sbeFullEnabled
        },
        {
            name: "GroupByNull_MaxWithoutIdAllPass",
            pipeline: [
                {$match: {[timeFieldName]: {$gt: dateLowerBound}}},
                {$group: {_id: null, a: {$max: '$y'}}},
                {$project: {_id: 0, a: 1}}
            ],
            usesBlockProcessing: sbeFullEnabled
        },
        {
            name: "GroupByNull_Max",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {$group: {_id: null, a: {$max: '$y'}}}
            ],
            usesBlockProcessing: sbeFullEnabled
        },
        {
            name: "GroupByNull_MinAndMaxWithoutId",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {$group: {_id: null, a: {$min: '$y'}, b: {$max: '$y'}}},
                {$project: {_id: 0, a: 1, b: 1}}
            ],
            usesBlockProcessing: sbeFullEnabled
        },
        {
            name: "GroupByNull_MinAndMaxWithoutIdAllPass",
            pipeline: [
                {$match: {[timeFieldName]: {$gt: dateLowerBound}}},
                {$group: {_id: null, a: {$min: '$y'}, b: {$max: '$y'}}},
                {$project: {_id: 0, a: 1, b: 1}}
            ],
            usesBlockProcessing: sbeFullEnabled
        },
        {
            name: "GroupByNull_MinAndMax",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {$group: {_id: null, a: {$min: '$y'}, b: {$max: '$y'}}}
            ],
            usesBlockProcessing: sbeFullEnabled
        },
        {
            name: "GroupByX_MinWithoutId",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {$group: {_id: '$x', a: {$min: '$y'}}},
                {$project: {_id: 0, a: 1}}
            ],
            usesBlockProcessing: sbeFullEnabled
        },
        {
            name: "GroupByX_Min",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {$group: {_id: '$x', a: {$min: '$y'}}}
            ],
            usesBlockProcessing: sbeFullEnabled
        },
        {
            name: "GroupByX_MaxWithoutId",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {$group: {_id: '$x', a: {$max: '$y'}}},
                {$project: {_id: 0, a: 1}}
            ],
            usesBlockProcessing: sbeFullEnabled
        },
        {
            name: "GroupByX_Max",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {$group: {_id: '$x', a: {$max: '$y'}}}
            ],
            usesBlockProcessing: sbeFullEnabled
        },
        {
            name: "GroupByX_MinAndMaxWithoutId",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {$group: {_id: '$x', a: {$min: '$y'}, b: {$max: '$y'}}},
                {$project: {_id: 0, a: 1, b: 1}}
            ],
            usesBlockProcessing: sbeFullEnabled
        },
        {
            name: "GroupByX_MinAndMax",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {$group: {_id: '$x', a: {$min: '$y'}, b: {$max: '$y'}}}
            ],
            usesBlockProcessing: sbeFullEnabled
        },
        {
            name: "GroupByDateTrunc_MinAndMax",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {
                    $group: {
                        _id: {$dateTrunc: {date: "$time", unit: "hour"}},
                        a: {$min: '$y'},
                        b: {$max: '$y'}
                    }
                },
                {$project: {_id: 1, a: 1, b: 1}}
            ],
            usesBlockProcessing: sbeFullEnabled
        },
        {
            name: "GroupByDateAdd_Min",
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
            name: "GroupByDateAddAndDateDiff_Min",
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
            name: "GroupByDateSubtract_Min",
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
            name: "GroupByDateSubtractAndDateDiff_Min",
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
            name: "GroupByNull_MaxAndMinOfDateDiff",
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
            name: "GroupByNull_MaxAndMinOfDateAddDateSubtractDateTrunc",
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
            name: "GroupByDateTruncAndDateAdd_MinAndMax",
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
                {$project: {_id: 1, a: 1, b: 1}}
            ],
            usesBlockProcessing: sbeFullEnabled
        },
        {
            name: "GroupByDateTruncAndDateSubtract_MinAndMax",
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
                {$project: {_id: 1, a: 1, b: 1}}
            ],
            usesBlockProcessing: sbeFullEnabled
        },
        {
            name: "GroupByDateDiffAndDateAdd_Min",
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
            name: "GroupByNull_MinOfDateDiff_InvalidDate",
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
            name: "GroupByNull_MinOfDateAdd_MissingAmount",
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
            name: "GroupByDateDiff_MinPlusMax",
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
                {
                    $project: {
                        _id: 1,
                        a: {
                            $cond: [
                                {$and: [{$isNumber: '$a'}, {$isNumber: '$b'}]},
                                {$add: ['$a', '$b']},
                                null
                            ]
                        }
                    }
                }
            ],
            usesBlockProcessing: sbeFullEnabled
        },
        {
            name: "GroupByFilteredComputedDateDiff_MinPlusMax",
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
                {
                    $project: {
                        _id: 1,
                        a: {
                            $cond: [
                                {$and: [{$isNumber: '$a'}, {$isNumber: '$b'}]},
                                {$add: ['$a', '$b']},
                                null
                            ]
                        }
                    }
                }
            ],
            usesBlockProcessing: sbeFullEnabled
        },
        {
            name: "GroupByX_MinWithoutId_NoFilter",
            pipeline: [{$group: {_id: '$x', a: {$min: '$y'}}}, {$project: {_id: 0, a: 1}}],
            usesBlockProcessing: sbeFullEnabled
        },
        {
            name: "GroupByX_Min_NoFilter",
            pipeline: [{$group: {_id: '$x', a: {$min: '$y'}}}],
            usesBlockProcessing: sbeFullEnabled
        },
        {
            name: "GroupByX_MaxWithoutId_NoFilter",
            pipeline: [{$group: {_id: '$x', a: {$max: '$y'}}}, {$project: {_id: 0, a: 1}}],
            usesBlockProcessing: sbeFullEnabled
        },
        {
            name: "GroupByX_Max_NoFilter",
            pipeline: [{$group: {_id: '$x', a: {$max: '$y'}}}],
            usesBlockProcessing: sbeFullEnabled
        },
        {
            name: "GroupByX_MinAndMaxWithoutId_NoFilter",
            pipeline: [
                {$group: {_id: '$x', a: {$min: '$y'}, b: {$max: '$y'}}},
                {$project: {_id: 0, a: 1, b: 1}}
            ],
            usesBlockProcessing: sbeFullEnabled
        },
        {
            name: "GroupByX_MinAndMax_NoFilter",
            pipeline: [{$group: {_id: '$x', a: {$min: '$y'}, b: {$max: '$y'}}}],
            usesBlockProcessing: sbeFullEnabled
        },
        {
            name: "GroupByDateTrunc_MinAndMax_NoFilter",
            pipeline: [
                {
                    $group: {
                        _id: {"$dateTrunc": {date: "$time", unit: "minute", binSize: 1}},
                        a: {$min: '$y'},
                        b: {$max: '$y'}
                    }
                },
                {$project: {_id: 1, a: 1, b: 1}}
            ],
            usesBlockProcessing: sbeFullEnabled
        },
        {
            name: "GroupByDateTruncAndDateDiff_MinAndMax_NoFilter",
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
                {$project: {_id: 1, a: 1, b: 1}}
            ],
            usesBlockProcessing: sbeFullEnabled
        },
        {
            name: "GroupByDateTruncAndMeta_MinAndMax_NoFilter",
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
                {$project: {_id: 1, a: 1, b: 1}}
            ],
            usesBlockProcessing: sbeFullEnabled
        },
        {
            name: "GroupByMeta_MinAndMax_NoFilter",
            pipeline: [
                {$group: {_id: "$measurement", a: {$min: '$y'}, b: {$max: '$y'}}},
                {$project: {_id: 1, a: 1, b: 1}}
            ],
            usesBlockProcessing: false
        },
        {
            name: "GroupByX_AvgWithoutId",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {$group: {_id: '$x', a: {$avg: '$y'}}},
                {$project: {_id: 0, a: 1}}
            ],
            usesBlockProcessing: false
        },
        {
            name: "GroupByXAndY_MinWithoutId",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {$group: {_id: {x: '$x', y: '$y'}, a: {$min: '$z'}}},
                {$project: {_id: 0, a: 1}}
            ],
            usesBlockProcessing: sbeFullEnabled
        },
        {
            name: "GroupByXAndY_Min",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {$group: {_id: {x: '$x', y: '$y'}, a: {$min: '$z'}}}
            ],
            usesBlockProcessing: sbeFullEnabled
        },
        {
            name: "GroupByXAndY_MaxWithoutId",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {$group: {_id: {x: '$x', y: '$y'}, a: {$max: '$z'}}},
                {$project: {_id: 0, a: 1}}
            ],
            usesBlockProcessing: sbeFullEnabled
        },
        {
            name: "GroupByXAndY_Max",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {$group: {_id: {x: '$x', y: '$y'}, a: {$max: '$z'}}}
            ],
            usesBlockProcessing: sbeFullEnabled
        },
        {
            name: "GroupByXAndY_MultipleMinsAndMaxs",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {
                    $group: {
                        _id: {x: '$x', y: '$y'},
                        a: {$min: '$z'},
                        b: {$max: '$z'},
                        c: {$min: '$p'},
                        d: {$max: '$q'}
                    }
                }
            ],
            usesBlockProcessing: sbeFullEnabled
        },
        {
            name: "GroupByMetaIndexKey_MinWithoutId",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {$group: {_id: {$meta: 'indexKey'}, a: {$min: '$y'}}},
                {$project: {_id: 0, a: 1}}
            ],
            usesBlockProcessing: false
        },
        {
            name: "GroupByX_MinOfMetaIndexKeyWithoutId",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {$group: {_id: '$x', a: {$min: {$meta: 'indexKey'}}}},
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
