/*
 * Test cases and common TS queries that use block processing.
 */
export function generateMetaVals() {
    const metaVals = ["foo", {region: "us", series: 123}, {region: "eu", series: 456}];
    return metaVals;
}

export function blockProcessingTestCases(timeFieldName,
                                         metaFieldName,
                                         datePrefix,
                                         dateUpperBound,
                                         dateLowerBound,
                                         featureFlagsAllowBlockHashAgg,
                                         sbeFullEnabled) {
    const dateMidPoint = new Date((dateLowerBound.getTime() + dateUpperBound.getTime()) / 2);
    const dollarPrefixedTime = '$' + timeFieldName;
    const dollarPrefixedMeta = '$' + metaFieldName;

    const metaDotRegion = dollarPrefixedMeta + '.region';
    const metaDotNonExistent = dollarPrefixedMeta + '.NON_EXISTENT';
    const metaDotSeries = dollarPrefixedMeta + '.series';

    return [
        {
            name: "GroupByNull",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {$group: {_id: null}},
                {$project: {_id: 1}}
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "Count",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {$count: "count"},
                {$project: {_id: 1, count: 1}}
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "Count_FilterHalf",
            pipeline: [
                {
                    $match: {
                        [timeFieldName]: {
                            $lt: new Date((dateLowerBound.getTime() + dateUpperBound.getTime()) / 2)
                        }
                    }
                },
                {$count: "count"},
                {$project: {_id: 1, count: 1}}
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByNull_MinWithoutId",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {$group: {_id: null, a: {$min: '$y'}}},
                {$project: {_id: 0, a: 1}}
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByNull_MinWithoutIdAllPass",
            pipeline: [
                {$match: {[timeFieldName]: {$gt: dateLowerBound}}},
                {$group: {_id: null, a: {$min: '$y'}}},
                {$project: {_id: 0, a: 1}}
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByNull_Min",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {$group: {_id: null, a: {$min: '$y'}}}
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByNull_MaxWithoutId",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {$group: {_id: null, a: {$max: '$y'}}},
                {$project: {_id: 0, a: 1}}
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByNull_MaxWithoutIdAllPass",
            pipeline: [
                {$match: {[timeFieldName]: {$gt: dateLowerBound}}},
                {$group: {_id: null, a: {$max: '$y'}}},
                {$project: {_id: 0, a: 1}}
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByNull_Max",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {$group: {_id: null, a: {$max: '$y'}}}
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByNull_MinAndMaxWithoutId",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {$group: {_id: null, a: {$min: '$y'}, b: {$max: '$y'}}},
                {$project: {_id: 0, a: 1, b: 1}}
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByNull_MinAndMaxWithoutIdAllPass",
            pipeline: [
                {$match: {[timeFieldName]: {$gt: dateLowerBound}}},
                {$group: {_id: null, a: {$min: '$y'}, b: {$max: '$y'}}},
                {$project: {_id: 0, a: 1, b: 1}}
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByNull_MinAndMax",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {$group: {_id: null, a: {$min: '$y'}, b: {$max: '$y'}}}
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByX_MinWithoutId",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {$group: {_id: '$x', a: {$min: '$y'}}},
                {$project: {_id: 0, a: 1}}
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByX_Min",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {$group: {_id: '$x', a: {$min: '$y'}}}
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByX_MaxWithoutId",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {$group: {_id: '$x', a: {$max: '$y'}}},
                {$project: {_id: 0, a: 1}}
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByX_Max",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {$group: {_id: '$x', a: {$max: '$y'}}}
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByX_MinAndMaxWithoutId",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {$group: {_id: '$x', a: {$min: '$y'}, b: {$max: '$y'}}},
                {$project: {_id: 0, a: 1, b: 1}}
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByX_MinAndMax",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {$group: {_id: '$x', a: {$min: '$y'}, b: {$max: '$y'}}}
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByDateTrunc_MinAndMax",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {
                    $group: {
                        _id: {$dateTrunc: {date: dollarPrefixedTime, unit: "hour"}},
                        a: {$min: '$y'},
                        b: {$max: '$y'}
                    }
                },
                {$project: {_id: 1, a: 1, b: 1}}
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByDateAdd_Min",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {
                    $group: {
                        _id: {
                            $dateAdd:
                                {startDate: dollarPrefixedTime, unit: "millisecond", amount: 100}
                        },
                        a: {$min: '$y'}
                    }
                },
                {$project: {_id: 1, a: '$a'}}
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
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
                                        endDate: dollarPrefixedTime,
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
                        _id: {
                            $dateSubtract:
                                {startDate: dollarPrefixedTime, unit: "millisecond", amount: 100}
                        },
                        a: {$min: '$y'}
                    }
                },
                {$project: {_id: 1, a: '$a'}}
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
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
                                        endDate: dollarPrefixedTime,
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
                                    endDate: dollarPrefixedTime,
                                    unit: "millisecond"
                                }
                            }
                        },
                        b: {
                            $max: {
                                $dateDiff: {
                                    startDate: new Date(datePrefix),
                                    endDate: dollarPrefixedTime,
                                    unit: "millisecond"
                                }
                            }
                        },
                        c: {
                            $min: {
                                $dateDiff: {
                                    startDate: dollarPrefixedTime,
                                    endDate: new Date(datePrefix),
                                    unit: "millisecond"
                                }
                            }
                        },
                        d: {
                            $max: {
                                $dateDiff: {
                                    startDate: dollarPrefixedTime,
                                    endDate: new Date(datePrefix),
                                    unit: "millisecond"
                                }
                            }
                        },
                    }
                },
                {$project: {_id: 0, a: 1, b: 1, c: 1, d: 1}}
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByNull_MaxAndMinOfDateAddDateSubtractDateTrunc",
            pipeline: [
                {$match: {[timeFieldName]: {$lte: dateUpperBound}}},
                {
                    $group: {
                        _id: null,
                        a: {
                            $min: {
                                $dateAdd: {
                                    startDate: dollarPrefixedTime,
                                    unit: "millisecond",
                                    amount: 100
                                }
                            }
                        },
                        b: {
                            $max: {
                                $dateSubtract: {
                                    startDate: dollarPrefixedTime,
                                    unit: "millisecond",
                                    amount: 100
                                }
                            }
                        },
                        c: {$max: {$dateTrunc: {date: dollarPrefixedTime, unit: "second"}}},
                    }
                },
                {$project: {_id: 0, a: 1, b: 1, c: 1}}
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
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
                                    $dateAdd: {
                                        startDate: dollarPrefixedTime,
                                        unit: "millisecond",
                                        amount: 100
                                    }
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
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
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
                                    $dateSubtract: {
                                        startDate: dollarPrefixedTime,
                                        unit: "millisecond",
                                        amount: 100
                                    }
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
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
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
                                    $dateAdd: {
                                        startDate: dollarPrefixedTime,
                                        unit: "millisecond",
                                        amount: 100
                                    }
                                },
                                unit: "millisecond"
                            }
                        },
                        a: {$min: '$y'},
                    }
                },
                {$project: {_id: 1, a: 1}}
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
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
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
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
                                endDate: dollarPrefixedTime,
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
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
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
                                endDate: dollarPrefixedTime,
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
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByX_MinWithoutId_NoFilter",
            pipeline: [{$group: {_id: '$x', a: {$min: '$y'}}}, {$project: {_id: 0, a: 1}}],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByX_Min_NoFilter",
            pipeline: [{$group: {_id: '$x', a: {$min: '$y'}}}],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByX_MaxWithoutId_NoFilter",
            pipeline: [{$group: {_id: '$x', a: {$max: '$y'}}}, {$project: {_id: 0, a: 1}}],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByX_Max_NoFilter",
            pipeline: [{$group: {_id: '$x', a: {$max: '$y'}}}],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByX_MinAndMaxWithoutId_NoFilter",
            pipeline: [
                {$group: {_id: '$x', a: {$min: '$y'}, b: {$max: '$y'}}},
                {$project: {_id: 0, a: 1, b: 1}}
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByX_MinAndMax_NoFilter",
            pipeline: [{$group: {_id: '$x', a: {$min: '$y'}, b: {$max: '$y'}}}],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByDateTrunc_MinAndMax_NoFilter",
            pipeline: [
                {
                    $group: {
                        _id: {"$dateTrunc": {date: dollarPrefixedTime, unit: "minute", binSize: 1}},
                        a: {$min: '$y'},
                        b: {$max: '$y'}
                    }
                },
                {$project: {_id: 1, a: 1, b: 1}}
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByDateTruncAndDateDiff_MinAndMax_NoFilter",
            pipeline: [
                {
                    $group: {
                        _id: {
                            date: {
                                $dateTrunc:
                                    {date: dollarPrefixedTime, unit: "millisecond", binSize: 200}
                            },
                            delta: {
                                $dateDiff: {
                                    startDate: new Date(datePrefix),
                                    endDate: dollarPrefixedTime,
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
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByDateTruncAndMeta_MinAndMax_NoFilter",
            pipeline: [
                {
                    $group: {
                        _id: {
                            date: {
                                $dateTrunc: {date: dollarPrefixedTime, unit: "minute", binSize: 1}
                            },
                            symbol: dollarPrefixedMeta
                        },
                        a: {$min: '$y'},
                        b: {$max: '$y'}
                    }
                },
                {$project: {_id: 1, a: 1, b: 1}}
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByMeta_MinAndMax_NoFilter",
            pipeline: [
                {$group: {_id: dollarPrefixedMeta, a: {$min: '$y'}, b: {$max: '$y'}}},
                {$project: {_id: 1, a: 1, b: 1}}
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
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByXAndY_Min",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {$group: {_id: {x: '$x', y: '$y'}, a: {$min: '$z'}}}
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByXAndY_MaxWithoutId",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {$group: {_id: {x: '$x', y: '$y'}, a: {$max: '$z'}}},
                {$project: {_id: 0, a: 1}}
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByXAndY_Max",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {$group: {_id: {x: '$x', y: '$y'}, a: {$max: '$z'}}}
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
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
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
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

        //
        // $sum tests //////////////////////////////////////////////////////////////////////////////
        //
        {
            name: "GroupByNull_Sum",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {$group: {_id: null, a: {$sum: '$y'}}}
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "DateUpper_GroupByNull_Sum_Project",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {$group: {_id: null, a: {$sum: '$y'}}},
                {$project: {_id: 0, a: 1}}
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "DateLower_GroupByNull_Sum_Project",
            pipeline: [
                {$match: {[timeFieldName]: {$gt: dateLowerBound}}},
                {$group: {_id: null, a: {$sum: '$y'}}},
                {$project: {_id: 0, a: 1}}
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByX_Sum",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {$group: {_id: '$x', a: {$sum: '$y'}}}
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByX_Sum_Project",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {$group: {_id: '$x', a: {$sum: '$y'}}},
                {$project: {_id: 0, a: 1}}
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByX_MultipleSums",
            pipeline: [{$group: {_id: '$x', a: {$sum: '$x'}, b: {$sum: '$y'}, c: {$sum: '$z'}}}],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByXAndY_Sum_Project",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {$group: {_id: {x: '$x', y: '$y'}, a: {$sum: '$z'}}},
                {$project: {_id: 0, a: 1}}
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByMetaSortKey_Sum_Project",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {$group: {_id: {$meta: 'sortKey'}, a: {$sum: '$y'}}},
                {$project: {_id: 0, a: 1}}
            ],
            usesBlockProcessing: false
        },
        {
            name: "GroupByX_SumOfMetaSortKey_Project",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {$group: {_id: '$x', a: {$sum: {$meta: 'sortKey'}}}},
                {$project: {_id: 0, a: 1}}
            ],
            usesBlockProcessing: false
        },

        //
        // $avg tests //////////////////////////////////////////////////////////////////////////////
        //
        {
            name: "GroupByNull_Avg",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {$group: {_id: null, a: {$avg: '$y'}}}
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "DateUpper_GroupByNull_Avg_Project",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {$group: {_id: null, a: {$avg: '$y'}}},
                {$project: {_id: 0, a: 1}}
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "DateLower_GroupByNull_Avg_Project",
            pipeline: [
                {$match: {[timeFieldName]: {$gt: dateLowerBound}}},
                {$group: {_id: null, a: {$avg: '$y'}}},
                {$project: {_id: 0, a: 1}}
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByX_Avg",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {$group: {_id: '$x', a: {$avg: '$y'}}}
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByX_Avg_Project",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {$group: {_id: '$x', a: {$avg: '$y'}}},
                {$project: {_id: 0, a: 1}}
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByX_MultipleAvgs",
            pipeline: [{$group: {_id: '$x', a: {$avg: '$x'}, b: {$avg: '$y'}, c: {$avg: '$z'}}}],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByXAndY_Avg_Project",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {$group: {_id: {x: '$x', y: '$y'}, a: {$avg: '$z'}}},
                {$project: {_id: 0, a: 1}}
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByMetaSortKey_Avg_Project",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {$group: {_id: {$meta: 'sortKey'}, a: {$avg: '$y'}}},
                {$project: {_id: 0, a: 1}}
            ],
            usesBlockProcessing: false
        },
        {
            name: "GroupByX_AvgOfMetaSortKey_Project",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {$group: {_id: '$x', a: {$avg: {$meta: 'sortKey'}}}},
                {$project: {_id: 0, a: 1}}
            ],
            usesBlockProcessing: false
        },

        //
        // Projected-out field tests ///////////////////////////////////////////////////////////////
        //
        {
            name: "MatchMetaGroupWithProjectedOutFieldInAccumulator",
            pipeline: [
                {$project: {_id: 0}},
                {$match: {[metaFieldName]: "foo"}},
                {$group: {_id: null, minY: {$min: "$y"}}},
            ],
            usesBlockProcessing: sbeFullEnabled
        },
        {
            name: "MatchTimeGroupWithProjectedOutFieldInAccumulator",
            pipeline: [
                {$project: {_id: 0}},
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
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
                {$project: {_id: 0, x: 1}},  // y not included
                {$match: {[metaFieldName]: "foo"}},
                {$group: {_id: "$y", a: {$min: "$x"}}},
            ],
            usesBlockProcessing: false
        },
        {
            name: "ProjectedOutCompoundGroupKeys",
            pipeline: [
                {$project: {t: "$t"}},
                {$project: {m: "$m"}},
                {$group: {_id: {t: "$t", m: "$m"}, gb: {$min: "$a"}}}
            ],
            usesBlockProcessing: sbeFullEnabled
        },
        {
            name: "GroupByNull_TopAndBottomSortByZ_OutputVariable",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {
                    $group: {
                        _id: null,
                        a: {$top: {sortBy: {z: 1}, output: "$w"}},
                        b: {$bottom: {sortBy: {z: 1}, output: "$w"}}
                    }
                }
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByNull_TopNAndBottomNSortByZ_OutputVariable",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {
                    $group: {
                        _id: null,
                        a: {$topN: {sortBy: {z: 1}, output: "$w", n: 3}},
                        b: {$bottomN: {sortBy: {z: 1}, output: "$w", n: 3}}
                    }
                }
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByNull_TopAndBottomSortByZ_OutputOneElemArray",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {
                    $group: {
                        _id: null,
                        a: {$top: {sortBy: {z: 1}, output: ["$w"]}},
                        b: {$bottom: {sortBy: {z: 1}, output: ["$w"]}}
                    }
                }
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByNull_TopNAndBottomNSortByZ_OutputOneElemArray",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {
                    $group: {
                        _id: null,
                        a: {$topN: {sortBy: {z: 1}, output: ["$w"], n: 3}},
                        b: {$bottomN: {sortBy: {z: 1}, output: ["$w"], n: 3}}
                    }
                }
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByNull_TopAndBottomSortByZ_OutputTwoElemArray",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {
                    $group: {
                        _id: null,
                        a: {$top: {sortBy: {z: 1}, output: ["$w", "$z"]}},
                        b: {$bottom: {sortBy: {z: 1}, output: ["$w", "$z"]}}
                    }
                }
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByNull_TopNAndBottomNSortByZ_OutputTwoElemArray",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {
                    $group: {
                        _id: null,
                        a: {$topN: {sortBy: {z: 1}, output: ["$w", "$z"], n: 3}},
                        b: {$bottomN: {sortBy: {z: 1}, output: ["$w", "$z"], n: 3}}
                    }
                }
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByX_TopAndBottomSortByZ_OutputVariable",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {
                    $group: {
                        _id: "$x",
                        a: {$top: {sortBy: {z: 1}, output: "$w"}},
                        b: {$bottom: {sortBy: {z: 1}, output: "$w"}}
                    }
                }
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByX_TopNAndBottomNSortByZ_OutputVariable",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {
                    $group: {
                        _id: "$x",
                        a: {$topN: {sortBy: {z: 1}, output: "$w", n: 3}},
                        b: {$bottomN: {sortBy: {z: 1}, output: "$w", n: 3}}
                    }
                }
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByX_TopAndBottomSortByZ_OutputOneElemArray",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {
                    $group: {
                        _id: "$x",
                        a: {$top: {sortBy: {z: 1}, output: ["$w"]}},
                        b: {$bottom: {sortBy: {z: 1}, output: ["$w"]}}
                    }
                }
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByX_TopNAndBottomNSortByZ_OutputOneElemArray",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {
                    $group: {
                        _id: "$x",
                        a: {$topN: {sortBy: {z: 1}, output: ["$w"], n: 3}},
                        b: {$bottomN: {sortBy: {z: 1}, output: ["$w"], n: 3}}
                    }
                }
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByX_TopAndBottomSortByZ_OutputTwoElemArray",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {
                    $group: {
                        _id: "$x",
                        a: {$top: {sortBy: {z: 1}, output: ["$w", "$z"]}},
                        b: {$bottom: {sortBy: {z: 1}, output: ["$w", "$z"]}}
                    }
                }
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByX_TopNAndBottomNSortByZ_OutputTwoElemArray",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {
                    $group: {
                        _id: "$x",
                        a: {$topN: {sortBy: {z: 1}, output: ["$w", "$z"], n: 3}},
                        b: {$bottomN: {sortBy: {z: 1}, output: ["$w", "$z"], n: 3}}
                    }
                }
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByXAndY_TopAndBottomSortByZ_OutputVariable",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {
                    $group: {
                        _id: {x: "$x", y: "$y"},
                        a: {$top: {sortBy: {z: 1}, output: "$w"}},
                        b: {$bottom: {sortBy: {z: 1}, output: "$w"}}
                    }
                }
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByXAndY_TopNAndBottomNSortByZ_OutputVariable",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {
                    $group: {
                        _id: {x: "$x", y: "$y"},
                        a: {$topN: {sortBy: {z: 1}, output: "$w", n: 3}},
                        b: {$bottomN: {sortBy: {z: 1}, output: "$w", n: 3}}
                    }
                }
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByXAndY_TopAndBottomSortByZ_OutputOneElemArray",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {
                    $group: {
                        _id: {x: "$x", y: "$y"},
                        a: {$top: {sortBy: {z: 1}, output: ["$w"]}},
                        b: {$bottom: {sortBy: {z: 1}, output: ["$w"]}}
                    }
                }
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByXAndY_TopNAndBottomNSortByZ_OutputOneElemArray",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {
                    $group: {
                        _id: {x: "$x", y: "$y"},
                        a: {$topN: {sortBy: {z: 1}, output: ["$w"], n: 3}},
                        b: {$bottomN: {sortBy: {z: 1}, output: ["$w"], n: 3}}
                    }
                }
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByXAndY_TopAndBottomSortByZ_OutputTwoElemArray",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {
                    $group: {
                        _id: {x: "$x", y: "$y"},
                        a: {$top: {sortBy: {z: 1}, output: ["$w", "$z"]}},
                        b: {$bottom: {sortBy: {z: 1}, output: ["$w", "$z"]}}
                    }
                }
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByXAndY_TopNAndBottomNSortByZ_OutputTwoElemArray",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {
                    $group: {
                        _id: {x: "$x", y: "$y"},
                        a: {$topN: {sortBy: {z: 1}, output: ["$w", "$z"], n: 3}},
                        b: {$bottomN: {sortBy: {z: 1}, output: ["$w", "$z"], n: 3}}
                    }
                }
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByNull_TopAndBottomSortByZAndW_OutputVariable",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {
                    $group: {
                        _id: null,
                        a: {$top: {sortBy: {z: 1, w: -1}, output: "$w"}},
                        b: {$bottom: {sortBy: {z: 1, w: -1}, output: "$w"}}
                    }
                }
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByNull_TopNAndBottomNSortByZAndW_OutputVariable",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {
                    $group: {
                        _id: null,
                        a: {$topN: {sortBy: {z: 1, w: -1}, output: "$w", n: 3}},
                        b: {$bottomN: {sortBy: {z: 1, w: -1}, output: "$w", n: 3}}
                    }
                }
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByNull_TopAndBottomSortByZAndW_OutputOneElemArray",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {
                    $group: {
                        _id: null,
                        a: {$top: {sortBy: {z: 1, w: -1}, output: ["$w"]}},
                        b: {$bottom: {sortBy: {z: 1, w: -1}, output: ["$w"]}}
                    }
                }
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByNull_TopNAndBottomNSortByZAndW_OutputOneElemArray",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {
                    $group: {
                        _id: null,
                        a: {$topN: {sortBy: {z: 1, w: -1}, output: ["$w"], n: 3}},
                        b: {$bottomN: {sortBy: {z: 1, w: -1}, output: ["$w"], n: 3}}
                    }
                }
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByNull_TopAndBottomSortByZAndW_OutputTwoElemArray",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {
                    $group: {
                        _id: null,
                        a: {$top: {sortBy: {z: 1, w: -1}, output: ["$w", "$z"]}},
                        b: {$bottom: {sortBy: {z: 1, w: -1}, output: ["$w", "$z"]}}
                    }
                }
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByNull_TopNAndBottomNSortByZAndW_OutputTwoElemArray",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {
                    $group: {
                        _id: null,
                        a: {$topN: {sortBy: {z: 1, w: -1}, output: ["$w", "$z"], n: 3}},
                        b: {$bottomN: {sortBy: {z: 1, w: -1}, output: ["$w", "$z"], n: 3}}
                    }
                }
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByX_TopAndBottomSortByZAndW_OutputVariable",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {
                    $group: {
                        _id: "$x",
                        a: {$top: {sortBy: {z: 1, w: -1}, output: "$w"}},
                        b: {$bottom: {sortBy: {z: 1, w: -1}, output: "$w"}}
                    }
                }
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByX_TopNAndBottomNSortByZAndW_OutputVariable",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {
                    $group: {
                        _id: "$x",
                        a: {$topN: {sortBy: {z: 1, w: -1}, output: "$w", n: 3}},
                        b: {$bottomN: {sortBy: {z: 1, w: -1}, output: "$w", n: 3}}
                    }
                }
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByX_TopAndBottomSortByZAndW_OutputOneElemArray",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {
                    $group: {
                        _id: "$x",
                        a: {$top: {sortBy: {z: 1, w: -1}, output: ["$w"]}},
                        b: {$bottom: {sortBy: {z: 1, w: -1}, output: ["$w"]}}
                    }
                }
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByX_TopNAndBottomNSortByZAndW_OutputOneElemArray",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {
                    $group: {
                        _id: "$x",
                        a: {$topN: {sortBy: {z: 1, w: -1}, output: ["$w"], n: 3}},
                        b: {$bottomN: {sortBy: {z: 1, w: -1}, output: ["$w"], n: 3}}
                    }
                }
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByX_TopAndBottomSortByZAndW_OutputTwoElemArray",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {
                    $group: {
                        _id: "$x",
                        a: {$top: {sortBy: {z: 1, w: -1}, output: ["$w", "$z"]}},
                        b: {$bottom: {sortBy: {z: 1, w: -1}, output: ["$w", "$z"]}}
                    }
                }
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByX_TopNAndBottomNSortByZAndW_OutputTwoElemArray",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {
                    $group: {
                        _id: "$x",
                        a: {$topN: {sortBy: {z: 1, w: -1}, output: ["$w", "$z"], n: 3}},
                        b: {$bottomN: {sortBy: {z: 1, w: -1}, output: ["$w", "$z"], n: 3}}
                    }
                }
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByXAndY_TopAndBottomSortByZAndW_OutputVariable",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {
                    $group: {
                        _id: {x: "$x", y: "$y"},
                        a: {$top: {sortBy: {z: 1, w: -1}, output: "$w"}},
                        b: {$bottom: {sortBy: {z: 1, w: -1}, output: "$w"}}
                    }
                }
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByXAndY_TopNAndBottomNSortByZAndW_OutputVariable",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {
                    $group: {
                        _id: {x: "$x", y: "$y"},
                        a: {$topN: {sortBy: {z: 1, w: -1}, output: "$w", n: 3}},
                        b: {$bottomN: {sortBy: {z: 1, w: -1}, output: "$w", n: 3}}
                    }
                }
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByXAndY_TopAndBottomSortByZAndW_OutputOneElemArray",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {
                    $group: {
                        _id: {x: "$x", y: "$y"},
                        a: {$top: {sortBy: {z: 1, w: -1}, output: ["$w"]}},
                        b: {$bottom: {sortBy: {z: 1, w: -1}, output: ["$w"]}}
                    }
                }
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByXAndY_TopNAndBottomNSortByZAndW_OutputOneElemArray",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {
                    $group: {
                        _id: {x: "$x", y: "$y"},
                        a: {$topN: {sortBy: {z: 1, w: -1}, output: ["$w"], n: 3}},
                        b: {$bottomN: {sortBy: {z: 1, w: -1}, output: ["$w"], n: 3}}
                    }
                }
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByXAndY_TopAndBottomSortByZAndW_OutputTwoElemArray",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {
                    $group: {
                        _id: {x: "$x", y: "$y"},
                        a: {$top: {sortBy: {z: 1, w: -1}, output: ["$w", "$z"]}},
                        b: {$bottom: {sortBy: {z: 1, w: -1}, output: ["$w", "$z"]}}
                    }
                }
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByXAndY_TopNAndBottomNSortByZAndW_OutputTwoElemArray",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {
                    $group: {
                        _id: {x: "$x", y: "$y"},
                        a: {$topN: {sortBy: {z: 1, w: -1}, output: ["$w", "$z"], n: 3}},
                        b: {$bottomN: {sortBy: {z: 1, w: -1}, output: ["$w", "$z"], n: 3}}
                    }
                }
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByNull_TopAndBottomSortByPAndQ_OutputVariable",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {
                    $group: {
                        _id: null,
                        a: {$top: {sortBy: {p: -1, q: 1}, output: "$p"}},
                        b: {$bottom: {sortBy: {p: -1, q: 1}, output: "$p"}}
                    }
                }
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByNull_TopNAndBottomNSortByPAndQ_OutputVariable",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {
                    $group: {
                        _id: null,
                        a: {$topN: {sortBy: {p: -1, q: 1}, output: "$p", n: 3}},
                        b: {$bottomN: {sortBy: {p: -1, q: 1}, output: "$p", n: 3}}
                    }
                }
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByNull_TopAndBottomSortByPAndQ_OutputOneElemArray",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {
                    $group: {
                        _id: null,
                        a: {$top: {sortBy: {p: -1, q: 1}, output: ["$p"]}},
                        b: {$bottom: {sortBy: {p: -1, q: 1}, output: ["$p"]}}
                    }
                }
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByNull_TopNAndBottomNSortByPAndQ_OutputOneElemArray",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {
                    $group: {
                        _id: null,
                        a: {$topN: {sortBy: {p: -1, q: 1}, output: ["$p"], n: 3}},
                        b: {$bottomN: {sortBy: {p: -1, q: 1}, output: ["$p"], n: 3}}
                    }
                }
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByNull_TopAndBottomSortByPAndQ_OutputTwoElemArray",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {
                    $group: {
                        _id: null,
                        a: {$top: {sortBy: {p: -1, q: 1}, output: ["$p", "$q"]}},
                        b: {$bottom: {sortBy: {p: -1, q: 1}, output: ["$p", "$q"]}}
                    }
                }
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByNull_TopNAndBottomNSortByPAndQ_OutputTwoElemArray",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {
                    $group: {
                        _id: null,
                        a: {$topN: {sortBy: {p: -1, q: 1}, output: ["$p", "$q"], n: 3}},
                        b: {$bottomN: {sortBy: {p: -1, q: 1}, output: ["$p", "$q"], n: 3}}
                    }
                }
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByX_TopAndBottomSortByPAndQ_OutputVariable",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {
                    $group: {
                        _id: "$x",
                        a: {$top: {sortBy: {p: -1, q: 1}, output: "$p"}},
                        b: {$bottom: {sortBy: {p: -1, q: 1}, output: "$p"}}
                    }
                }
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByX_TopNAndBottomNSortByPAndQ_OutputVariable",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {
                    $group: {
                        _id: "$x",
                        a: {$topN: {sortBy: {p: -1, q: 1}, output: "$p", n: 3}},
                        b: {$bottomN: {sortBy: {p: -1, q: 1}, output: "$p", n: 3}}
                    }
                }
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByX_TopAndBottomSortByPAndQ_OutputOneElemArray",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {
                    $group: {
                        _id: "$x",
                        a: {$top: {sortBy: {p: -1, q: 1}, output: ["$p"]}},
                        b: {$bottom: {sortBy: {p: -1, q: 1}, output: ["$p"]}}
                    }
                }
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByX_TopNAndBottomNSortByPAndQ_OutputOneElemArray",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {
                    $group: {
                        _id: "$x",
                        a: {$topN: {sortBy: {p: -1, q: 1}, output: ["$p"], n: 3}},
                        b: {$bottomN: {sortBy: {p: -1, q: 1}, output: ["$p"], n: 3}}
                    }
                }
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByX_TopAndBottomSortByPAndQ_OutputTwoElemArray",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {
                    $group: {
                        _id: "$x",
                        a: {$top: {sortBy: {p: -1, q: 1}, output: ["$p", "$q"]}},
                        b: {$bottom: {sortBy: {p: -1, q: 1}, output: ["$p", "$q"]}}
                    }
                }
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByX_TopNAndBottomNSortByPAndQ_OutputTwoElemArray",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {
                    $group: {
                        _id: "$x",
                        a: {$topN: {sortBy: {p: -1, q: 1}, output: ["$p", "$q"], n: 3}},
                        b: {$bottomN: {sortBy: {p: -1, q: 1}, output: ["$p", "$q"], n: 3}}
                    }
                }
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByXAndY_TopAndBottomSortByPAndQ_OutputVariable",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {
                    $group: {
                        _id: {x: "$x", y: "$y"},
                        a: {$top: {sortBy: {p: -1, q: 1}, output: "$p"}},
                        b: {$bottom: {sortBy: {p: -1, q: 1}, output: "$p"}}
                    }
                }
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByXAndY_TopNAndBottomNSortByPAndQ_OutputVariable",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {
                    $group: {
                        _id: {x: "$x", y: "$y"},
                        a: {$topN: {sortBy: {p: -1, q: 1}, output: "$p", n: 3}},
                        b: {$bottomN: {sortBy: {p: -1, q: 1}, output: "$p", n: 3}}
                    }
                }
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByXAndY_TopAndBottomSortByPAndQ_OutputOneElemArray",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {
                    $group: {
                        _id: {x: "$x", y: "$y"},
                        a: {$top: {sortBy: {p: -1, q: 1}, output: ["$p"]}},
                        b: {$bottom: {sortBy: {p: -1, q: 1}, output: ["$p"]}}
                    }
                }
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByXAndY_TopNAndBottomNSortByPAndQ_OutputOneElemArray",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {
                    $group: {
                        _id: {x: "$x", y: "$y"},
                        a: {$topN: {sortBy: {p: -1, q: 1}, output: ["$p"], n: 3}},
                        b: {$bottomN: {sortBy: {p: -1, q: 1}, output: ["$p"], n: 3}}
                    }
                }
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByXAndY_TopAndBottomSortByPAndQ_OutputTwoElemArray",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {
                    $group: {
                        _id: {x: "$x", y: "$y"},
                        a: {$top: {sortBy: {p: -1, q: 1}, output: ["$p", "$q"]}},
                        b: {$bottom: {sortBy: {p: -1, q: 1}, output: ["$p", "$q"]}}
                    }
                }
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByXAndY_TopNAndBottomNSortByPAndQ_OutputTwoElemArray",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {
                    $group: {
                        _id: {x: "$x", y: "$y"},
                        a: {$topN: {sortBy: {p: -1, q: 1}, output: ["$p", "$q"], n: 3}},
                        b: {$bottomN: {sortBy: {p: -1, q: 1}, output: ["$p", "$q"], n: 3}}
                    }
                }
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByXAndY_TopNAndBottomNSortByZ_OutputEmptyArray",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {
                    $group: {
                        _id: {x: "$x", y: "$y"},
                        a: {$topN: {sortBy: {z: 1}, output: [], n: 3}},
                        b: {$bottomN: {sortBy: {z: 1}, output: [], n: 3}}
                    }
                }
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByXAndY_TopNAndBottomNSortByZAndW_OutputEmptyArray",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateUpperBound}}},
                {
                    $group: {
                        _id: {x: "$x", y: "$y"},
                        a: {$topN: {sortBy: {z: 1, w: -1}, output: [], n: 3}},
                        b: {$bottomN: {sortBy: {z: 1, w: -1}, output: [], n: 3}}
                    }
                }
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByMetaSubField",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateMidPoint}}},
                {$group: {_id: metaDotRegion, a: {$min: "$x"}}},
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByMetaSubFields",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateMidPoint}}},
                {$group: {_id: {region: metaDotRegion, series: metaDotSeries}, a: {$min: "$x"}}},
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByMetaMultipleSubFields",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateMidPoint}}},
                {$group: {_id: {region: metaDotRegion, series: metaDotSeries}, a: {$min: "$x"}}},
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByMetaSubFieldExpression",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateMidPoint}}},
                {
                    $group: {
                        _id: {region: {$ifNull: [metaDotRegion, "foo"]}, series: metaDotSeries},
                        a: {$min: "$x"}
                    }
                },
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByIfNullAlwaysNull",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateMidPoint}}},
                {
                    $group: {
                        _id: {gb: {$ifNull: ['$x', '$z', null]}, series: metaDotSeries},
                        a: {$sum: "$y"}
                    }
                },
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByIfNullDifferentFinalType",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateMidPoint}}},
                {
                    $group: {
                        _id:
                            {gb: {$ifNull: ["$x", "$z", NumberDecimal(37)]}, series: metaDotSeries},
                        a: {$sum: "$y"}
                    }
                },
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByIfNullNeverNull",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateMidPoint}}},
                {
                    $group: {
                        _id: {gb: {$ifNull: ["$p", "$q", "$z"]}, series: metaDotSeries},
                        a: {$max: "$x"}
                    }
                },
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByAccumulateIfNullSum",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateMidPoint}}},
                {$group: {_id: {gb: metaDotRegion}, a: {$sum: {$ifNull: ["$z", "$y", "$p"]}}}},
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByAccumulateIfNullMinMax",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateMidPoint}}},
                {
                    $group: {
                        _id: {gb: metaDotRegion},
                        a: {$min: {$ifNull: ["$z", 6]}},
                        b: {$max: {$ifNull: ["$y", "$missing", -4]}}
                    }
                },
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByAccumulateIfNullManyArgs",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateMidPoint}}},
                {
                    $group: {
                        _id: {gb: metaDotRegion},
                        a: {
                            $min: {
                                $ifNull: [
                                    "$z",
                                    "$missing1",
                                    null,
                                    "$missing2",
                                    "$missing3",
                                    "$y",
                                    "$missing1",
                                    [1, 2, 3]
                                ]
                            }
                        },
                    }
                },
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByMetaNonExistent",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateMidPoint}}},
                {$group: {_id: metaDotNonExistent, a: {$min: "$x"}}},
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByExpressionOnMetaNonExistent",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateMidPoint}}},
                {$group: {_id: {$toLower: metaDotNonExistent}, a: {$min: "$x"}}},
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByMetaAndMeasurement",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateMidPoint}}},
                {$group: {_id: {region: metaDotRegion, y: "$y"}, a: {$min: "$x"}}},
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByMetaSubFieldAndTime",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateMidPoint}}},
                {
                    $group: {
                        _id: {
                            region: metaDotRegion,
                            time: {$dateTrunc: {date: dollarPrefixedTime, unit: "hour"}}
                        },
                        a: {$min: "$x"},
                        b: {$max: "$y"}
                    }
                },
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByXMinOfMeta",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateMidPoint}}},
                {$group: {_id: "$x", a: {$min: dollarPrefixedMeta}}},
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByXMinOfMetaSubField",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateMidPoint}}},
                {$group: {_id: "$x", a: {$min: metaDotSeries}}},
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByXMinOfExpressionOfMeta",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateMidPoint}}},
                {$group: {_id: "$x", a: {$min: {$toLower: metaDotSeries}}}},
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByXAvgOfMeta",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateMidPoint}}},
                {$group: {_id: "$x", a: {$avg: dollarPrefixedMeta}}},
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByXAvgOfMetaSubField",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateMidPoint}}},
                {$group: {_id: "$x", a: {$avg: metaDotSeries}}},
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByComputedField",
            pipeline: [
                {
                    $addFields: {
                        computedField: {
                            $cond: [
                                {$and: [{$isNumber: '$a'}, {$isNumber: '$b'}]},
                                {$add: ['$a', '$b']},
                                null
                            ]
                        }
                    }
                },
                {$match: {[timeFieldName]: {$lt: dateMidPoint}, computedField: 999}},
                {$group: {_id: "$computedField", a: {$min: "$y"}}},
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
        {
            name: "GroupByWithComplexIdExpr",
            pipeline: [
                {$match: {[timeFieldName]: {$lt: dateMidPoint}}},
                {
                    $group: {
                        _id: {$lt: [29336, {$mod: ["$b", 1.0]}]},
                    }
                }
            ],
            usesBlockProcessing: featureFlagsAllowBlockHashAgg
        },
    ];
}
