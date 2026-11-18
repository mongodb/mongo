## 1. group: dead and live accumulators
### Pipeline
```json
[
	{
		"$group" : {
			"_id" : "$k",
			"dead" : {
				"$push" : {
					"$multiply" : [
						"$unusedInput",
						2
					]
				}
			},
			"total" : {
				"$sum" : "$x"
			}
		}
	},
	{
		"$project" : {
			"_id" : 0,
			"total" : 1
		}
	}
]
```
### Options
```json
{ "allowDiskUse" : true, "hint" : { "$natural" : 1 } }
```
### Without blueprint rewrites
Results:
```text
{ "total" : 3 }
{ "total" : 6 }
```
SBE stages:
```text
[4] project [s13 = newBsonObj("total", s12)] 
[3] project [s12 = doubleDoubleSumFinalize(s9)] 
[3] group [s10] [s11 = s10] [s6 = _internalPush::accumulate(s6, 
    if isNullish(s4) 
    then null 
    elif isNumber(s4) 
    then (s4 * 2L) 
    else fail(7157721, "only numbers are allowed in an $multiply expression") 
), s9 = aggDoubleDoubleSum(s5)] spillSlots[s7, s8] mergingExprs[_internalPush::merge(s7), aggMergeDoubleDoubleSums(s8)] 
[3] project [s10 = (s3 ?: null)] 
[1] scan generic [s1 = record, s2 = recordId] [s3 = k, s4 = unusedInput, s5 = x] @"UUID" 
```
### With blueprint rewrites
Results:
```text
{ "total" : 3 }
{ "total" : 6 }
```
SBE stages:
```text
[4] project [s13 = newBsonObj("total", s12)] 
[3] project [s12 = doubleDoubleSumFinalize(s9)] 
[3] group [s10] [s11 = s10] [s9 = aggDoubleDoubleSum(s5)] spillSlots[s8] mergingExprs[aggMergeDoubleDoubleSums(s8)] 
[3] project [s10 = (s3 ?: null)] 
[1] scan generic [s1 = record, s2 = recordId] [s3 = k, s5 = x] @"UUID" 
```

## 2. group: all accumulators dead, unread group key
### Pipeline
```json
[
	{
		"$group" : {
			"_id" : "$k",
			"dead" : {
				"$push" : {
					"$multiply" : [
						"$unusedInput",
						2
					]
				}
			}
		}
	},
	{
		"$project" : {
			"_id" : 0,
			"ok" : {
				"$literal" : 1
			}
		}
	}
]
```
### Options
```json
{ "allowDiskUse" : true, "hint" : { "$natural" : 1 } }
```
### Without blueprint rewrites
Results:
```text
{ "ok" : 1 }
{ "ok" : 1 }
```
SBE stages:
```text
[4] project [s10 = newBsonObj("ok", s9)] 
[4] project [s9 = 1L] 
[3] group [s7] [s8 = s7] [s5 = _internalPush::accumulate(s5, 
    if isNullish(s4) 
    then null 
    elif isNumber(s4) 
    then (s4 * 2L) 
    else fail(7157721, "only numbers are allowed in an $multiply expression") 
)] spillSlots[s6] mergingExprs[_internalPush::merge(s6)] 
[3] project [s7 = (s3 ?: null)] 
[1] scan generic [s1 = record, s2 = recordId] [s3 = k, s4 = unusedInput] @"UUID" 
```
### With blueprint rewrites
Results:
```text
{ "ok" : 1 }
{ "ok" : 1 }
```
SBE stages:
```text
[4] project [s10 = newBsonObj("ok", s9)] 
[4] project [s9 = 1L] 
[3] group [s7] [s8 = s7] [] spillSlots[] mergingExprs[] 
[3] project [s7 = (s3 ?: null)] 
[1] scan generic [s1 = record, s2 = recordId] [s3 = k] @"UUID" 
```

## 3. group: all accumulators dead, global group
### Pipeline
```json
[
	{
		"$group" : {
			"_id" : null,
			"dead" : {
				"$push" : {
					"$multiply" : [
						"$unusedInput",
						2
					]
				}
			}
		}
	},
	{
		"$project" : {
			"_id" : 0,
			"ok" : {
				"$literal" : 1
			}
		}
	}
]
```
### Options
```json
{ "allowDiskUse" : true, "hint" : { "$natural" : 1 } }
```
### Without blueprint rewrites
Results:
```text
{ "ok" : 1 }
```
SBE stages:
```text
[4] project [s8 = newBsonObj("ok", s7)] 
[4] project [s7 = 1L] 
[3] project [s6 = null] 
[3] group [] [] [s4 = _internalPush::accumulate(s4, 
    if isNullish(s3) 
    then null 
    elif isNumber(s3) 
    then (s3 * 2L) 
    else fail(7157721, "only numbers are allowed in an $multiply expression") 
)] spillSlots[s5] mergingExprs[_internalPush::merge(s5)] 
[1] scan generic [s1 = record, s2 = recordId] [s3 = unusedInput] @"UUID" 
```
### With blueprint rewrites
Results:
```text
{ "ok" : 1 }
```
SBE stages:
```text
[4] project [s8 = newBsonObj("ok", s7)] 
[4] project [s7 = 1L] 
[3] group [] [] [] spillSlots[] mergingExprs[] 
[1] scan generic [s1 = record, s2 = recordId] @"UUID" 
```

## 4. group: all accumulators dead, empty input
### Pipeline
```json
[
	{
		"$match" : {
			"k" : 42
		}
	},
	{
		"$group" : {
			"_id" : null,
			"dead" : {
				"$push" : {
					"$multiply" : [
						"$unusedInput",
						2
					]
				}
			}
		}
	},
	{
		"$project" : {
			"_id" : 0,
			"ok" : {
				"$literal" : 1
			}
		}
	}
]
```
### Options
```json
{ "allowDiskUse" : true, "hint" : { "$natural" : 1 } }
```
### Without blueprint rewrites
Results:
```text

```
SBE stages:
```text
[4] project [s9 = newBsonObj("ok", s8)] 
[4] project [s8 = 1L] 
[3] project [s7 = null] 
[3] group [] [] [s5 = _internalPush::accumulate(s5, 
    if isNullish(s3) 
    then null 
    elif isNumber(s3) 
    then (s3 * 2L) 
    else fail(7157721, "only numbers are allowed in an $multiply expression") 
)] spillSlots[s6] mergingExprs[_internalPush::merge(s6)] 
[1] filter {((s4 == 42L) ?: false)} 
[1] scan generic [s1 = record, s2 = recordId] [s3 = unusedInput, s4 = k] @"UUID" 
```
### With blueprint rewrites
Results:
```text

```
SBE stages:
```text
[4] project [s9 = newBsonObj("ok", s8)] 
[4] project [s8 = 1L] 
[3] group [] [] [] spillSlots[] mergingExprs[] 
[1] filter {((s4 == 42L) ?: false)} 
[1] scan generic [s1 = record, s2 = recordId] [s4 = k] @"UUID" 
```

## 5. compact_group: dead and live accumulators
### Pipeline
```json
[
	{
		"$group" : {
			"_id" : "$k",
			"dead" : {
				"$push" : {
					"$multiply" : [
						"$unusedInput",
						2
					]
				}
			},
			"total" : {
				"$sum" : "$x"
			}
		}
	},
	{
		"$project" : {
			"_id" : 0,
			"total" : 1
		}
	}
]
```
### Options
```json
{ "allowDiskUse" : false, "hint" : { "$natural" : 1 } }
```
### Without blueprint rewrites
Results:
```text
{ "total" : 3 }
{ "total" : 6 }
```
SBE stages:
```text
[4] project [s12 = newBsonObj("total", s8)] 
[3] compact_group [s10] [s11 = s10] [s6 = _compactPush(
    if isNullish(s4) 
    then null 
    elif isNumber(s4) 
    then (s4 * 2L) 
    else fail(7157721, "only numbers are allowed in an $multiply expression") 
), s8 = _compactSum(s5)] 
[3] project [s10 = (s3 ?: null)] 
[1] scan generic [s1 = record, s2 = recordId] [s3 = k, s4 = unusedInput, s5 = x] @"UUID" 
```
### With blueprint rewrites
Results:
```text
{ "total" : 3 }
{ "total" : 6 }
```
SBE stages:
```text
[4] project [s12 = newBsonObj("total", s8)] 
[3] compact_group [s10] [s11 = s10] [s8 = _compactSum(s5)] 
[3] project [s10 = (s3 ?: null)] 
[1] scan generic [s1 = record, s2 = recordId] [s3 = k, s5 = x] @"UUID" 
```

## 6. compact_group: all accumulators dead, unread group key
### Pipeline
```json
[
	{
		"$group" : {
			"_id" : "$k",
			"dead" : {
				"$push" : {
					"$multiply" : [
						"$unusedInput",
						2
					]
				}
			}
		}
	},
	{
		"$project" : {
			"_id" : 0,
			"ok" : {
				"$literal" : 1
			}
		}
	}
]
```
### Options
```json
{ "allowDiskUse" : false, "hint" : { "$natural" : 1 } }
```
### Without blueprint rewrites
Results:
```text
{ "ok" : 1 }
{ "ok" : 1 }
```
SBE stages:
```text
[4] project [s10 = newBsonObj("ok", s9)] 
[4] project [s9 = 1L] 
[3] compact_group [s7] [s8 = s7] [s5 = _compactPush(
    if isNullish(s4) 
    then null 
    elif isNumber(s4) 
    then (s4 * 2L) 
    else fail(7157721, "only numbers are allowed in an $multiply expression") 
)] 
[3] project [s7 = (s3 ?: null)] 
[1] scan generic [s1 = record, s2 = recordId] [s3 = k, s4 = unusedInput] @"UUID" 
```
### With blueprint rewrites
Results:
```text
{ "ok" : 1 }
{ "ok" : 1 }
```
SBE stages:
```text
[4] project [s10 = newBsonObj("ok", s9)] 
[4] project [s9 = 1L] 
[3] compact_group [s7] [s8 = s7] [] 
[3] project [s7 = (s3 ?: null)] 
[1] scan generic [s1 = record, s2 = recordId] [s3 = k] @"UUID" 
```

## 7. compact_group: all accumulators dead, global group
### Pipeline
```json
[
	{
		"$group" : {
			"_id" : null,
			"dead" : {
				"$push" : {
					"$multiply" : [
						"$unusedInput",
						2
					]
				}
			}
		}
	},
	{
		"$project" : {
			"_id" : 0,
			"ok" : {
				"$literal" : 1
			}
		}
	}
]
```
### Options
```json
{ "allowDiskUse" : false, "hint" : { "$natural" : 1 } }
```
### Without blueprint rewrites
Results:
```text
{ "ok" : 1 }
```
SBE stages:
```text
[4] project [s8 = newBsonObj("ok", s7)] 
[4] project [s7 = 1L] 
[3] project [s6 = null] 
[3] compact_group [] [] [s4 = _compactPush(
    if isNullish(s3) 
    then null 
    elif isNumber(s3) 
    then (s3 * 2L) 
    else fail(7157721, "only numbers are allowed in an $multiply expression") 
)] 
[1] scan generic [s1 = record, s2 = recordId] [s3 = unusedInput] @"UUID" 
```
### With blueprint rewrites
Results:
```text
{ "ok" : 1 }
```
SBE stages:
```text
[4] project [s8 = newBsonObj("ok", s7)] 
[4] project [s7 = 1L] 
[3] compact_group [] [] [] 
[1] scan generic [s1 = record, s2 = recordId] @"UUID" 
```

## 8. compact_group: all accumulators dead, empty input
### Pipeline
```json
[
	{
		"$match" : {
			"k" : 42
		}
	},
	{
		"$group" : {
			"_id" : null,
			"dead" : {
				"$push" : {
					"$multiply" : [
						"$unusedInput",
						2
					]
				}
			}
		}
	},
	{
		"$project" : {
			"_id" : 0,
			"ok" : {
				"$literal" : 1
			}
		}
	}
]
```
### Options
```json
{ "allowDiskUse" : false, "hint" : { "$natural" : 1 } }
```
### Without blueprint rewrites
Results:
```text

```
SBE stages:
```text
[4] project [s9 = newBsonObj("ok", s8)] 
[4] project [s8 = 1L] 
[3] project [s7 = null] 
[3] compact_group [] [] [s5 = _compactPush(
    if isNullish(s3) 
    then null 
    elif isNumber(s3) 
    then (s3 * 2L) 
    else fail(7157721, "only numbers are allowed in an $multiply expression") 
)] 
[1] filter {((s4 == 42L) ?: false)} 
[1] scan generic [s1 = record, s2 = recordId] [s3 = unusedInput, s4 = k] @"UUID" 
```
### With blueprint rewrites
Results:
```text

```
SBE stages:
```text
[4] project [s9 = newBsonObj("ok", s8)] 
[4] project [s8 = 1L] 
[3] compact_group [] [] [] 
[1] filter {((s4 == 42L) ?: false)} 
[1] scan generic [s1 = record, s2 = recordId] [s4 = k] @"UUID" 
```

## 9. streaming_group: dead and live accumulators
### Pipeline
```json
[
	{
		"$group" : {
			"_id" : "$k",
			"dead" : {
				"$push" : {
					"$multiply" : [
						"$unusedInput",
						2
					]
				}
			},
			"total" : {
				"$sum" : "$x"
			}
		}
	},
	{
		"$project" : {
			"_id" : 0,
			"total" : 1
		}
	}
]
```
### Options
```json
{ "hint" : { "k" : 1 } }
```
### Without blueprint rewrites
Results:
```text
{ "total" : 3 }
{ "total" : 6 }
```
SBE stages:
```text
[5] project [s18 = newBsonObj("total", s17)] 
[4] project [s17 = doubleDoubleSumFinalize(s14)] 
[4] streaming_group [s15] [s16 = s15] clustered [s15] [s11 = _internalPush::accumulate(s11, 
    if isNullish(s9) 
    then null 
    elif isNumber(s9) 
    then (s9 * 2L) 
    else fail(7157721, "only numbers are allowed in an $multiply expression") 
), s14 = aggDoubleDoubleSum(s10)] 
[4] project [s15 = (s8 ?: null)] 
[2] fetch s1 = seek, s6 = result, s7 = recordId, s4 = inSnapshotId, s2 = inIndexIdent, s3 = inIndexKey, s5 = inIndexKeyPattern [s8 = k, s9 = unusedInput, s10 = x] @"UUID" 
[1] ixseek seekKeyLow = KS(0A01) seekKeyHigh = KS(F0FE) [s3 = indexKey, s1 = recordId, s4 = snapshotId, s2 = indexIdent] [] @"UUID" @"k_1" forward 
```
### With blueprint rewrites
Results:
```text
{ "total" : 3 }
{ "total" : 6 }
```
SBE stages:
```text
[5] project [s18 = newBsonObj("total", s17)] 
[4] project [s17 = doubleDoubleSumFinalize(s14)] 
[4] streaming_group [s15] [s16 = s15] clustered [s15] [s14 = aggDoubleDoubleSum(s10)] 
[4] project [s15 = (s8 ?: null)] 
[2] fetch s1 = seek, s6 = result, s7 = recordId, s4 = inSnapshotId, s2 = inIndexIdent, s3 = inIndexKey, s5 = inIndexKeyPattern [s8 = k, s9 = unusedInput, s10 = x] @"UUID" 
[1] ixseek seekKeyLow = KS(0A01) seekKeyHigh = KS(F0FE) [s3 = indexKey, s1 = recordId, s4 = snapshotId, s2 = indexIdent] [] @"UUID" @"k_1" forward 
```

## 10. streaming_group: all accumulators dead, unread group key
### Pipeline
```json
[
	{
		"$group" : {
			"_id" : "$k",
			"dead" : {
				"$push" : {
					"$multiply" : [
						"$unusedInput",
						2
					]
				}
			}
		}
	},
	{
		"$project" : {
			"_id" : 0,
			"ok" : {
				"$literal" : 1
			}
		}
	}
]
```
### Options
```json
{ "hint" : { "k" : 1 } }
```
### Without blueprint rewrites
Results:
```text
{ "ok" : 1 }
{ "ok" : 1 }
```
SBE stages:
```text
[5] project [s15 = newBsonObj("ok", s14)] 
[5] project [s14 = 1L] 
[4] streaming_group [s12] [s13 = s12] clustered [s12] [s10 = _internalPush::accumulate(s10, 
    if isNullish(s9) 
    then null 
    elif isNumber(s9) 
    then (s9 * 2L) 
    else fail(7157721, "only numbers are allowed in an $multiply expression") 
)] 
[4] project [s12 = (s8 ?: null)] 
[2] fetch s1 = seek, s6 = result, s7 = recordId, s4 = inSnapshotId, s2 = inIndexIdent, s3 = inIndexKey, s5 = inIndexKeyPattern [s8 = k, s9 = unusedInput] @"UUID" 
[1] ixseek seekKeyLow = KS(0A01) seekKeyHigh = KS(F0FE) [s3 = indexKey, s1 = recordId, s4 = snapshotId, s2 = indexIdent] [] @"UUID" @"k_1" forward 
```
### With blueprint rewrites
Results:
```text
{ "ok" : 1 }
{ "ok" : 1 }
```
SBE stages:
```text
[5] project [s15 = newBsonObj("ok", s14)] 
[5] project [s14 = 1L] 
[4] streaming_group [s12] [s13 = s12] clustered [s12] [] 
[4] project [s12 = (s8 ?: null)] 
[2] fetch s1 = seek, s6 = result, s7 = recordId, s4 = inSnapshotId, s2 = inIndexIdent, s3 = inIndexKey, s5 = inIndexKeyPattern [s8 = k, s9 = unusedInput] @"UUID" 
[1] ixseek seekKeyLow = KS(0A01) seekKeyHigh = KS(F0FE) [s3 = indexKey, s1 = recordId, s4 = snapshotId, s2 = indexIdent] [] @"UUID" @"k_1" forward 
```

## 11. streaming_group: all accumulators dead, global group
### Pipeline
```json
[
	{
		"$group" : {
			"_id" : null,
			"dead" : {
				"$push" : {
					"$multiply" : [
						"$unusedInput",
						2
					]
				}
			}
		}
	},
	{
		"$project" : {
			"_id" : 0,
			"ok" : {
				"$literal" : 1
			}
		}
	}
]
```
### Options
```json
{ "hint" : { "k" : 1 } }
```
### Without blueprint rewrites
Results:
```text
{ "ok" : 1 }
```
SBE stages:
```text
[5] project [s13 = newBsonObj("ok", s12)] 
[5] project [s12 = 1L] 
[4] project [s11 = null] 
[4] group [] [] [s9 = _internalPush::accumulate(s9, 
    if isNullish(s8) 
    then null 
    elif isNumber(s8) 
    then (s8 * 2L) 
    else fail(7157721, "only numbers are allowed in an $multiply expression") 
)] spillSlots[s10] mergingExprs[_internalPush::merge(s10)] 
[2] fetch s1 = seek, s6 = result, s7 = recordId, s4 = inSnapshotId, s2 = inIndexIdent, s3 = inIndexKey, s5 = inIndexKeyPattern [s8 = unusedInput] @"UUID" 
[1] ixseek seekKeyLow = KS(0A01) seekKeyHigh = KS(F0FE) [s3 = indexKey, s1 = recordId, s4 = snapshotId, s2 = indexIdent] [] @"UUID" @"k_1" forward 
```
### With blueprint rewrites
Results:
```text
{ "ok" : 1 }
```
SBE stages:
```text
[5] project [s13 = newBsonObj("ok", s12)] 
[5] project [s12 = 1L] 
[4] group [] [] [] spillSlots[] mergingExprs[] 
[2] fetch s1 = seek, s6 = result, s7 = recordId, s4 = inSnapshotId, s2 = inIndexIdent, s3 = inIndexKey, s5 = inIndexKeyPattern [s8 = unusedInput] @"UUID" 
[1] ixseek seekKeyLow = KS(0A01) seekKeyHigh = KS(F0FE) [s3 = indexKey, s1 = recordId, s4 = snapshotId, s2 = indexIdent] [] @"UUID" @"k_1" forward 
```

## 12. streaming_group: all accumulators dead, empty input
### Pipeline
```json
[
	{
		"$match" : {
			"k" : 42
		}
	},
	{
		"$group" : {
			"_id" : null,
			"dead" : {
				"$push" : {
					"$multiply" : [
						"$unusedInput",
						2
					]
				}
			}
		}
	},
	{
		"$project" : {
			"_id" : 0,
			"ok" : {
				"$literal" : 1
			}
		}
	}
]
```
### Options
```json
{ "hint" : { "k" : 1 } }
```
### Without blueprint rewrites
Results:
```text

```
SBE stages:
```text
[5] project [s13 = newBsonObj("ok", s12)] 
[5] project [s12 = 1L] 
[4] project [s11 = null] 
[4] group [] [] [s9 = _internalPush::accumulate(s9, 
    if isNullish(s8) 
    then null 
    elif isNumber(s8) 
    then (s8 * 2L) 
    else fail(7157721, "only numbers are allowed in an $multiply expression") 
)] spillSlots[s10] mergingExprs[_internalPush::merge(s10)] 
[2] fetch s1 = seek, s6 = result, s7 = recordId, s4 = inSnapshotId, s2 = inIndexIdent, s3 = inIndexKey, s5 = inIndexKeyPattern [s8 = unusedInput] @"UUID" 
[1] ixseek seekKeyLow = KS(2B5401) seekKeyHigh = KS(2B54FE) [s3 = indexKey, s1 = recordId, s4 = snapshotId, s2 = indexIdent] [] @"UUID" @"k_1" forward 
```
### With blueprint rewrites
Results:
```text

```
SBE stages:
```text
[5] project [s13 = newBsonObj("ok", s12)] 
[5] project [s12 = 1L] 
[4] group [] [] [] spillSlots[] mergingExprs[] 
[2] fetch s1 = seek, s6 = result, s7 = recordId, s4 = inSnapshotId, s2 = inIndexIdent, s3 = inIndexKey, s5 = inIndexKeyPattern [s8 = unusedInput] @"UUID" 
[1] ixseek seekKeyLow = KS(2B5401) seekKeyHigh = KS(2B54FE) [s3 = indexKey, s1 = recordId, s4 = snapshotId, s2 = indexIdent] [] @"UUID" @"k_1" forward 
```

