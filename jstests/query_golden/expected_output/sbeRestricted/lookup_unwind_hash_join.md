## 1. Unwind: { "$unwind" : "$joined" }

### Pipeline
```json
[
	{
		"$lookup" : {
			"from" : "foreign",
			"localField" : "lkey",
			"foreignField" : "fkey",
			"as" : "joined"
		}
	},
	{
		"$unwind" : "$joined"
	},
	{
		"$addFields" : {
			"computed" : {
				"$add" : [
					"$a",
					"$joined.b"
				]
			}
		}
	}
]
```
### Options
```json
{ "allowDiskUse" : true }
```
### Results
```text
{ "_id" : 0, "lkey" : 1, "a" : 5, "blob" : "x", "joined" : { "_id" : 10, "fkey" : 1, "b" : 10 }, "computed" : 15 }
{ "_id" : 0, "lkey" : 1, "a" : 5, "blob" : "x", "joined" : { "_id" : 11, "fkey" : 1, "b" : 20 }, "computed" : 25 }
{ "_id" : 2, "lkey" : [ 1, 3 ], "a" : 9, "blob" : "y", "joined" : { "_id" : 10, "fkey" : 1, "b" : 10 }, "computed" : 19 }
{ "_id" : 2, "lkey" : [ 1, 3 ], "a" : 9, "blob" : "y", "joined" : { "_id" : 11, "fkey" : 1, "b" : 20 }, "computed" : 29 }
{ "_id" : 2, "lkey" : [ 1, 3 ], "a" : 9, "blob" : "y", "joined" : { "_id" : 12, "fkey" : 3, "b" : 30 }, "computed" : 39 }
{ "_id" : 3, "joined" : { "_id" : 12, "fkey" : 3, "b" : 30 }, "lkey" : 3, "computed" : null }
{ "_id" : 4, "a" : 1, "joined" : { "_id" : 13, "fkey" : null, "b" : 40 }, "computed" : 41 }
{ "_id" : 5, "lkey" : null, "blob" : "z", "joined" : { "_id" : 13, "fkey" : null, "b" : 40 }, "computed" : null }
```
### Plan
Engine: SBE
Strategy: HashJoin
Slots: $$RESULT=s15 env: {  }
```text
[4] project [s15 = makeBsonObj(MakeObjSpec([joined = Set(0), computed = Set(1)], Open, NewObj, 0), s1, s12, s14)] 
[4] project [s14 = 
    if (isNullish(s3) || isNullish(s13)) 
    then null 
    elif ((
        if isNumber(s3) 
        then 0 
        elif isDate(s3) 
        then 1 
        else fail(7157723, "only numbers and dates are allowed in an $add expression") 
    + 
        if isNumber(s13) 
        then 0 
        elif isDate(s13) 
        then 1 
        else fail(7157723, "only numbers and dates are allowed in an $add expression") 
   ) > 1) 
    then fail(7157722, "only one date allowed in an $add expression") 
    else (s3 + s13) 
] 
[4] extract_field_paths inputs[s3 = Get(a)/Id, s12 = Get(joined)/Id] outputs[s13 = Get(joined)/Traverse/Get(b)/Id] 
[3] hash_lookup_unwind inner s12 
    outer s10 
        [3] project [s10 = arrayToSet(
            if (isArray(s4) ?: false) 
            then 
                if isArrayEmpty(s4) 
                then [null] 
                else s4 
            
            else newArray((s4 ?: null)) 
       )] 
        [1] scan generic [s1 = record, s2 = recordId] [s3 = a, s4 = lkey] @"" 
    inner s11 s5 
        [3] project [s11 = arrayToSet(
            let [
                l3.0 = (s8 ?: null) 
            ] 
            in 
                if isArray(l3.0) 
                then concatArrays(l3.0, newArray(l3.0)) 
                else newArray(move(l3.0)) 
       )] 
        [2] scan generic [s5 = record, s6 = recordId] [s7 = a, s8 = fkey, s9 = joined] @"" 
```

### Pipeline
```json
[
	{
		"$lookup" : {
			"from" : "foreign",
			"localField" : "lkey",
			"foreignField" : "fkey",
			"as" : "joined"
		}
	},
	{
		"$unwind" : "$joined"
	},
	{
		"$project" : {
			"_id" : 0,
			"lkey" : 1,
			"joined" : 1
		}
	}
]
```
### Options
```json
{ "allowDiskUse" : true }
```
### Results
```text
{ "lkey" : 1, "joined" : { "_id" : 10, "fkey" : 1, "b" : 10 } }
{ "lkey" : 1, "joined" : { "_id" : 11, "fkey" : 1, "b" : 20 } }
{ "lkey" : [ 1, 3 ], "joined" : { "_id" : 10, "fkey" : 1, "b" : 10 } }
{ "lkey" : [ 1, 3 ], "joined" : { "_id" : 11, "fkey" : 1, "b" : 20 } }
{ "lkey" : [ 1, 3 ], "joined" : { "_id" : 12, "fkey" : 3, "b" : 30 } }
{ "lkey" : 3, "joined" : { "_id" : 12, "fkey" : 3, "b" : 30 } }
{ "joined" : { "_id" : 13, "fkey" : null, "b" : 40 } }
{ "lkey" : null, "joined" : { "_id" : 13, "fkey" : null, "b" : 40 } }
```
### Plan
Engine: classic

## 2. Unwind: { "$unwind" : { "path" : "$joined", "preserveNullAndEmptyArrays" : true } }

### Pipeline
```json
[
	{
		"$lookup" : {
			"from" : "foreign",
			"localField" : "lkey",
			"foreignField" : "fkey",
			"as" : "joined"
		}
	},
	{
		"$unwind" : {
			"path" : "$joined",
			"preserveNullAndEmptyArrays" : true
		}
	},
	{
		"$addFields" : {
			"computed" : {
				"$add" : [
					"$a",
					"$joined.b"
				]
			}
		}
	}
]
```
### Options
```json
{ "allowDiskUse" : true }
```
### Results
```text
{ "_id" : 0, "lkey" : 1, "a" : 5, "blob" : "x", "joined" : { "_id" : 10, "fkey" : 1, "b" : 10 }, "computed" : 15 }
{ "_id" : 0, "lkey" : 1, "a" : 5, "blob" : "x", "joined" : { "_id" : 11, "fkey" : 1, "b" : 20 }, "computed" : 25 }
{ "_id" : 1, "lkey" : 2, "a" : 7, "computed" : null }
{ "_id" : 2, "lkey" : [ 1, 3 ], "a" : 9, "blob" : "y", "joined" : { "_id" : 10, "fkey" : 1, "b" : 10 }, "computed" : 19 }
{ "_id" : 2, "lkey" : [ 1, 3 ], "a" : 9, "blob" : "y", "joined" : { "_id" : 11, "fkey" : 1, "b" : 20 }, "computed" : 29 }
{ "_id" : 2, "lkey" : [ 1, 3 ], "a" : 9, "blob" : "y", "joined" : { "_id" : 12, "fkey" : 3, "b" : 30 }, "computed" : 39 }
{ "_id" : 3, "joined" : { "_id" : 12, "fkey" : 3, "b" : 30 }, "lkey" : 3, "computed" : null }
{ "_id" : 4, "a" : 1, "joined" : { "_id" : 13, "fkey" : null, "b" : 40 }, "computed" : 41 }
{ "_id" : 5, "lkey" : null, "blob" : "z", "joined" : { "_id" : 13, "fkey" : null, "b" : 40 }, "computed" : null }
```
### Plan
Engine: SBE
Strategy: HashJoin
Slots: $$RESULT=s19 env: {  }
```text
[4] project [s19 = makeBsonObj(MakeObjSpec([computed = Set(0)], Open, NewObj, 0), s14, s18)] 
[4] project [s18 = 
    if (isNullish(s15) || isNullish(s17)) 
    then null 
    elif ((
        if isNumber(s15) 
        then 0 
        elif isDate(s15) 
        then 1 
        else fail(7157723, "only numbers and dates are allowed in an $add expression") 
    + 
        if isNumber(s17) 
        then 0 
        elif isDate(s17) 
        then 1 
        else fail(7157723, "only numbers and dates are allowed in an $add expression") 
   ) > 1) 
    then fail(7157722, "only one date allowed in an $add expression") 
    else (s15 + s17) 
] 
[4] extract_field_paths inputs[s15 = Get(a)/Id, s16 = Get(joined)/Id] outputs[s17 = Get(joined)/Traverse/Get(b)/Id] 
[3] project [s15 = getField(s14, "a"), s16 = getField(s14, "joined")] 
[3] project [s14 = makeObj(MakeObjSpec([joined = Set(0)], Open, NewObj, 0), s1, s13)] 
[3] hash_lookup_unwind left s13 
    outer s11 
        [3] project [s11 = arrayToSet(
            if (isArray(s5) ?: false) 
            then 
                if isArrayEmpty(s5) 
                then [null] 
                else s5 
            
            else newArray((s5 ?: null)) 
       )] 
        [1] scan generic [s1 = record, s2 = recordId] [s3 = a, s4 = joined, s5 = lkey] @"" 
    inner s12 s6 
        [3] project [s12 = arrayToSet(
            let [
                l3.0 = (s9 ?: null) 
            ] 
            in 
                if isArray(l3.0) 
                then concatArrays(l3.0, newArray(l3.0)) 
                else newArray(move(l3.0)) 
       )] 
        [2] scan generic [s6 = record, s7 = recordId] [s8 = a, s9 = fkey, s10 = joined] @"" 
```

### Pipeline
```json
[
	{
		"$lookup" : {
			"from" : "foreign",
			"localField" : "lkey",
			"foreignField" : "fkey",
			"as" : "joined"
		}
	},
	{
		"$unwind" : {
			"path" : "$joined",
			"preserveNullAndEmptyArrays" : true
		}
	},
	{
		"$project" : {
			"_id" : 0,
			"lkey" : 1,
			"joined" : 1
		}
	}
]
```
### Options
```json
{ "allowDiskUse" : true }
```
### Results
```text
{ "lkey" : 1, "joined" : { "_id" : 10, "fkey" : 1, "b" : 10 } }
{ "lkey" : 1, "joined" : { "_id" : 11, "fkey" : 1, "b" : 20 } }
{ "lkey" : 2 }
{ "lkey" : [ 1, 3 ], "joined" : { "_id" : 10, "fkey" : 1, "b" : 10 } }
{ "lkey" : [ 1, 3 ], "joined" : { "_id" : 11, "fkey" : 1, "b" : 20 } }
{ "lkey" : [ 1, 3 ], "joined" : { "_id" : 12, "fkey" : 3, "b" : 30 } }
{ "lkey" : 3, "joined" : { "_id" : 12, "fkey" : 3, "b" : 30 } }
{ "joined" : { "_id" : 13, "fkey" : null, "b" : 40 } }
{ "lkey" : null, "joined" : { "_id" : 13, "fkey" : null, "b" : 40 } }
```
### Plan
Engine: classic

## 3. Unwind: { "$unwind" : { "path" : "$joined", "includeArrayIndex" : "idx" } }

### Pipeline
```json
[
	{
		"$lookup" : {
			"from" : "foreign",
			"localField" : "lkey",
			"foreignField" : "fkey",
			"as" : "joined"
		}
	},
	{
		"$unwind" : {
			"path" : "$joined",
			"includeArrayIndex" : "idx"
		}
	},
	{
		"$addFields" : {
			"computed" : {
				"$add" : [
					"$a",
					"$joined.b"
				]
			}
		}
	}
]
```
### Options
```json
{ "allowDiskUse" : true }
```
### Results
```text
{ "_id" : 0, "lkey" : 1, "a" : 5, "blob" : "x", "joined" : { "_id" : 10, "fkey" : 1, "b" : 10 }, "idx" : NumberLong(0), "computed" : 15 }
{ "_id" : 0, "lkey" : 1, "a" : 5, "blob" : "x", "joined" : { "_id" : 11, "fkey" : 1, "b" : 20 }, "idx" : NumberLong(1), "computed" : 25 }
{ "_id" : 2, "lkey" : [ 1, 3 ], "a" : 9, "blob" : "y", "joined" : { "_id" : 10, "fkey" : 1, "b" : 10 }, "idx" : NumberLong(0), "computed" : 19 }
{ "_id" : 2, "lkey" : [ 1, 3 ], "a" : 9, "blob" : "y", "joined" : { "_id" : 11, "fkey" : 1, "b" : 20 }, "idx" : NumberLong(1), "computed" : 29 }
{ "_id" : 2, "lkey" : [ 1, 3 ], "a" : 9, "blob" : "y", "joined" : { "_id" : 12, "fkey" : 3, "b" : 30 }, "idx" : NumberLong(2), "computed" : 39 }
{ "_id" : 3, "joined" : { "_id" : 12, "fkey" : 3, "b" : 30 }, "lkey" : 3, "idx" : NumberLong(0), "computed" : null }
{ "_id" : 4, "a" : 1, "joined" : { "_id" : 13, "fkey" : null, "b" : 40 }, "idx" : NumberLong(0), "computed" : 41 }
{ "_id" : 5, "lkey" : null, "blob" : "z", "joined" : { "_id" : 13, "fkey" : null, "b" : 40 }, "idx" : NumberLong(0), "computed" : null }
```
### Plan
Engine: SBE
Strategy: HashJoin
Slots: $$RESULT=s20 env: {  }
```text
[4] project [s20 = makeBsonObj(MakeObjSpec([computed = Set(0)], Open, NewObj, 0), s15, s19)] 
[4] project [s19 = 
    if (isNullish(s16) || isNullish(s18)) 
    then null 
    elif ((
        if isNumber(s16) 
        then 0 
        elif isDate(s16) 
        then 1 
        else fail(7157723, "only numbers and dates are allowed in an $add expression") 
    + 
        if isNumber(s18) 
        then 0 
        elif isDate(s18) 
        then 1 
        else fail(7157723, "only numbers and dates are allowed in an $add expression") 
   ) > 1) 
    then fail(7157722, "only one date allowed in an $add expression") 
    else (s16 + s18) 
] 
[4] extract_field_paths inputs[s16 = Get(a)/Id, s17 = Get(joined)/Id] outputs[s18 = Get(joined)/Traverse/Get(b)/Id] 
[3] project [s16 = getField(s15, "a"), s17 = getField(s15, "joined")] 
[3] project [s15 = makeObj(MakeObjSpec([joined = Set(0), idx = Set(1)], Open, NewObj, 0), s1, s14, s6)] 
[3] hash_lookup_unwind inner s14 s6 
    outer s12 
        [3] project [s12 = arrayToSet(
            if (isArray(s5) ?: false) 
            then 
                if isArrayEmpty(s5) 
                then [null] 
                else s5 
            
            else newArray((s5 ?: null)) 
       )] 
        [1] scan generic [s1 = record, s2 = recordId] [s3 = a, s4 = joined, s5 = lkey] @"" 
    inner s13 s7 
        [3] project [s13 = arrayToSet(
            let [
                l3.0 = (s10 ?: null) 
            ] 
            in 
                if isArray(l3.0) 
                then concatArrays(l3.0, newArray(l3.0)) 
                else newArray(move(l3.0)) 
       )] 
        [2] scan generic [s7 = record, s8 = recordId] [s9 = a, s10 = fkey, s11 = joined] @"" 
```

### Pipeline
```json
[
	{
		"$lookup" : {
			"from" : "foreign",
			"localField" : "lkey",
			"foreignField" : "fkey",
			"as" : "joined"
		}
	},
	{
		"$unwind" : {
			"path" : "$joined",
			"includeArrayIndex" : "idx"
		}
	},
	{
		"$project" : {
			"_id" : 0,
			"lkey" : 1,
			"joined" : 1
		}
	}
]
```
### Options
```json
{ "allowDiskUse" : true }
```
### Results
```text
{ "lkey" : 1, "joined" : { "_id" : 10, "fkey" : 1, "b" : 10 } }
{ "lkey" : 1, "joined" : { "_id" : 11, "fkey" : 1, "b" : 20 } }
{ "lkey" : [ 1, 3 ], "joined" : { "_id" : 10, "fkey" : 1, "b" : 10 } }
{ "lkey" : [ 1, 3 ], "joined" : { "_id" : 11, "fkey" : 1, "b" : 20 } }
{ "lkey" : [ 1, 3 ], "joined" : { "_id" : 12, "fkey" : 3, "b" : 30 } }
{ "lkey" : 3, "joined" : { "_id" : 12, "fkey" : 3, "b" : 30 } }
{ "joined" : { "_id" : 13, "fkey" : null, "b" : 40 } }
{ "lkey" : null, "joined" : { "_id" : 13, "fkey" : null, "b" : 40 } }
```
### Plan
Engine: classic

## 4. Dotted as path, unwind: { "$unwind" : "$a.b" }

### Pipeline
```json
[
	{
		"$lookup" : {
			"from" : "foreignDotted",
			"localField" : "lkey",
			"foreignField" : "fkey",
			"as" : "a.b"
		}
	},
	{
		"$unwind" : "$a.b"
	}
]
```
### Options
```json
{ "allowDiskUse" : true }
```
### Results
```text
{ "_id" : 0, "lkey" : 1, "a" : { "x" : 10, "b" : { "_id" : 10, "fkey" : 1, "c" : 100 } } }
{ "_id" : 0, "lkey" : 1, "a" : { "x" : 10, "b" : { "_id" : 11, "fkey" : 1, "c" : 200 } } }
{ "_id" : 2, "lkey" : [ 1, 3 ], "a" : { "b" : { "_id" : 10, "fkey" : 1, "c" : 100 } } }
{ "_id" : 2, "lkey" : [ 1, 3 ], "a" : { "b" : { "_id" : 11, "fkey" : 1, "c" : 200 } } }
{ "_id" : 2, "lkey" : [ 1, 3 ], "a" : { "b" : { "_id" : 12, "fkey" : 3, "c" : 300 } } }
{ "_id" : 3, "lkey" : 3, "a" : { "b" : { "_id" : 12, "fkey" : 3, "c" : 300 } } }
{ "_id" : 4, "a" : { "x" : 1, "b" : { "_id" : 13, "fkey" : null, "c" : 400 } } }
{ "_id" : 5, "lkey" : null, "a" : { "b" : { "_id" : 13, "fkey" : null, "c" : 400 } } }
```
### Plan
Engine: SBE
Strategy: HashJoin
Slots: $$RESULT=s10 env: {  }
```text
[3] project [s10 = makeBsonObj(MakeObjSpec([a = MakeObj([b = Set(0)], Open, NewObj, 0)], Open, NewObj, 0), s1, s9)] 
[3] hash_lookup_unwind inner s9 
    outer s7 
        [3] project [s7 = arrayToSet(
            if (isArray(s3) ?: false) 
            then 
                if isArrayEmpty(s3) 
                then [null] 
                else s3 
            
            else newArray((s3 ?: null)) 
       )] 
        [1] scan generic [s1 = record, s2 = recordId] [s3 = lkey] @"" 
    inner s8 s4 
        [3] project [s8 = arrayToSet(
            let [
                l3.0 = (s6 ?: null) 
            ] 
            in 
                if isArray(l3.0) 
                then concatArrays(l3.0, newArray(l3.0)) 
                else newArray(move(l3.0)) 
       )] 
        [2] scan generic [s4 = record, s5 = recordId] [s6 = fkey] @"" 
```

### Pipeline
```json
[
	{
		"$lookup" : {
			"from" : "foreignDotted",
			"localField" : "lkey",
			"foreignField" : "fkey",
			"as" : "a.b"
		}
	},
	{
		"$unwind" : "$a.b"
	},
	{
		"$match" : {
			"a" : {
				"$exists" : true
			}
		}
	}
]
```
### Options
```json
{ "allowDiskUse" : true }
```
### Results
```text
{ "_id" : 0, "lkey" : 1, "a" : { "x" : 10, "b" : { "_id" : 10, "fkey" : 1, "c" : 100 } } }
{ "_id" : 0, "lkey" : 1, "a" : { "x" : 10, "b" : { "_id" : 11, "fkey" : 1, "c" : 200 } } }
{ "_id" : 2, "lkey" : [ 1, 3 ], "a" : { "b" : { "_id" : 10, "fkey" : 1, "c" : 100 } } }
{ "_id" : 2, "lkey" : [ 1, 3 ], "a" : { "b" : { "_id" : 11, "fkey" : 1, "c" : 200 } } }
{ "_id" : 2, "lkey" : [ 1, 3 ], "a" : { "b" : { "_id" : 12, "fkey" : 3, "c" : 300 } } }
{ "_id" : 3, "lkey" : 3, "a" : { "b" : { "_id" : 12, "fkey" : 3, "c" : 300 } } }
{ "_id" : 4, "a" : { "x" : 1, "b" : { "_id" : 13, "fkey" : null, "c" : 400 } } }
{ "_id" : 5, "lkey" : null, "a" : { "b" : { "_id" : 13, "fkey" : null, "c" : 400 } } }
```
### Plan
Engine: SBE
Strategy: HashJoin
Slots: $$RESULT=s12 env: {  }
```text
[4] filter {exists(s13)} 
[3] project [s13 = getField(s12, "a")] 
[3] project [s12 = makeObj(MakeObjSpec([a = MakeObj([b = Set(0)], Open, NewObj, 0)], Open, NewObj, 0), s1, s11)] 
[3] hash_lookup_unwind inner s11 
    outer s9 
        [3] project [s9 = arrayToSet(
            if (isArray(s4) ?: false) 
            then 
                if isArrayEmpty(s4) 
                then [null] 
                else s4 
            
            else newArray((s4 ?: null)) 
       )] 
        [1] scan generic [s1 = record, s2 = recordId] [s3 = a, s4 = lkey] @"" 
    inner s10 s5 
        [3] project [s10 = arrayToSet(
            let [
                l3.0 = (s8 ?: null) 
            ] 
            in 
                if isArray(l3.0) 
                then concatArrays(l3.0, newArray(l3.0)) 
                else newArray(move(l3.0)) 
       )] 
        [2] scan generic [s5 = record, s6 = recordId] [s7 = a, s8 = fkey] @"" 
```

### Pipeline
```json
[
	{
		"$lookup" : {
			"from" : "foreignDotted",
			"localField" : "lkey",
			"foreignField" : "fkey",
			"as" : "a.b"
		}
	},
	{
		"$unwind" : "$a.b"
	},
	{
		"$match" : {
			"a.b" : {
				"$exists" : true
			}
		}
	}
]
```
### Options
```json
{ "allowDiskUse" : true }
```
### Results
```text
{ "_id" : 0, "lkey" : 1, "a" : { "x" : 10, "b" : { "_id" : 10, "fkey" : 1, "c" : 100 } } }
{ "_id" : 0, "lkey" : 1, "a" : { "x" : 10, "b" : { "_id" : 11, "fkey" : 1, "c" : 200 } } }
{ "_id" : 2, "lkey" : [ 1, 3 ], "a" : { "b" : { "_id" : 10, "fkey" : 1, "c" : 100 } } }
{ "_id" : 2, "lkey" : [ 1, 3 ], "a" : { "b" : { "_id" : 11, "fkey" : 1, "c" : 200 } } }
{ "_id" : 2, "lkey" : [ 1, 3 ], "a" : { "b" : { "_id" : 12, "fkey" : 3, "c" : 300 } } }
{ "_id" : 3, "lkey" : 3, "a" : { "b" : { "_id" : 12, "fkey" : 3, "c" : 300 } } }
{ "_id" : 4, "a" : { "x" : 1, "b" : { "_id" : 13, "fkey" : null, "c" : 400 } } }
{ "_id" : 5, "lkey" : null, "a" : { "b" : { "_id" : 13, "fkey" : null, "c" : 400 } } }
```
### Plan
Engine: SBE
Strategy: HashJoin
Slots: $$RESULT=s12 env: {  }
```text
[4] filter {traverseF(s13, lambda(l6.0) { exists(getField(move(l6.0), "b")) }, false)} 
[3] project [s13 = getField(s12, "a")] 
[3] project [s12 = makeObj(MakeObjSpec([a = MakeObj([b = Set(0)], Open, NewObj, 0)], Open, NewObj, 0), s1, s11)] 
[3] hash_lookup_unwind inner s11 
    outer s9 
        [3] project [s9 = arrayToSet(
            if (isArray(s4) ?: false) 
            then 
                if isArrayEmpty(s4) 
                then [null] 
                else s4 
            
            else newArray((s4 ?: null)) 
       )] 
        [1] scan generic [s1 = record, s2 = recordId] [s3 = a, s4 = lkey] @"" 
    inner s10 s5 
        [3] project [s10 = arrayToSet(
            let [
                l3.0 = (s8 ?: null) 
            ] 
            in 
                if isArray(l3.0) 
                then concatArrays(l3.0, newArray(l3.0)) 
                else newArray(move(l3.0)) 
       )] 
        [2] scan generic [s5 = record, s6 = recordId] [s7 = a, s8 = fkey] @"" 
```

### Pipeline
```json
[
	{
		"$lookup" : {
			"from" : "foreignDotted",
			"localField" : "lkey",
			"foreignField" : "fkey",
			"as" : "a.b"
		}
	},
	{
		"$unwind" : "$a.b"
	},
	{
		"$match" : {
			"a.b.c" : {
				"$gt" : 150
			}
		}
	}
]
```
### Options
```json
{ "allowDiskUse" : true }
```
### Results
```text
{ "_id" : 0, "lkey" : 1, "a" : { "x" : 10, "b" : { "_id" : 11, "fkey" : 1, "c" : 200 } } }
{ "_id" : 2, "lkey" : [ 1, 3 ], "a" : { "b" : { "_id" : 11, "fkey" : 1, "c" : 200 } } }
{ "_id" : 2, "lkey" : [ 1, 3 ], "a" : { "b" : { "_id" : 12, "fkey" : 3, "c" : 300 } } }
{ "_id" : 3, "lkey" : 3, "a" : { "b" : { "_id" : 12, "fkey" : 3, "c" : 300 } } }
{ "_id" : 4, "a" : { "x" : 1, "b" : { "_id" : 13, "fkey" : null, "c" : 400 } } }
{ "_id" : 5, "lkey" : null, "a" : { "b" : { "_id" : 13, "fkey" : null, "c" : 400 } } }
```
### Plan
Engine: classic

### Pipeline
```json
[
	{
		"$lookup" : {
			"from" : "foreignDotted",
			"localField" : "lkey",
			"foreignField" : "fkey",
			"as" : "a.b"
		}
	},
	{
		"$unwind" : "$a.b"
	},
	{
		"$match" : {
			"a" : {
				"$exists" : true
			},
			"a.b.c" : {
				"$gt" : 150
			}
		}
	}
]
```
### Options
```json
{ "allowDiskUse" : true }
```
### Results
```text
{ "_id" : 0, "lkey" : 1, "a" : { "x" : 10, "b" : { "_id" : 11, "fkey" : 1, "c" : 200 } } }
{ "_id" : 2, "lkey" : [ 1, 3 ], "a" : { "b" : { "_id" : 11, "fkey" : 1, "c" : 200 } } }
{ "_id" : 2, "lkey" : [ 1, 3 ], "a" : { "b" : { "_id" : 12, "fkey" : 3, "c" : 300 } } }
{ "_id" : 3, "lkey" : 3, "a" : { "b" : { "_id" : 12, "fkey" : 3, "c" : 300 } } }
{ "_id" : 4, "a" : { "x" : 1, "b" : { "_id" : 13, "fkey" : null, "c" : 400 } } }
{ "_id" : 5, "lkey" : null, "a" : { "b" : { "_id" : 13, "fkey" : null, "c" : 400 } } }
```
### Plan
Engine: SBE
Strategy: HashJoin
Slots: $$RESULT=s12 env: {  }
```text
[4] filter {(exists(s13) && traverseF(s13, lambda(l8.0) { traverseF(getField(move(l8.0), "b"), lambda(l9.0) { traverseF(getField(move(l9.0), "c"), lambda(l10.0) { ((move(l10.0) > 150L) ?: false) }, false) }, false) }, false))} 
[3] project [s13 = getField(s12, "a")] 
[3] project [s12 = makeObj(MakeObjSpec([a = MakeObj([b = Set(0)], Open, NewObj, 0)], Open, NewObj, 0), s1, s11)] 
[3] hash_lookup_unwind inner s11 
    outer s9 
        [3] project [s9 = arrayToSet(
            if (isArray(s4) ?: false) 
            then 
                if isArrayEmpty(s4) 
                then [null] 
                else s4 
            
            else newArray((s4 ?: null)) 
       )] 
        [1] scan generic [s1 = record, s2 = recordId] [s3 = a, s4 = lkey] @"" 
    inner s10 s5 
        [3] project [s10 = arrayToSet(
            let [
                l3.0 = (s8 ?: null) 
            ] 
            in 
                if isArray(l3.0) 
                then concatArrays(l3.0, newArray(l3.0)) 
                else newArray(move(l3.0)) 
       )] 
        [2] scan generic [s5 = record, s6 = recordId] [s7 = a, s8 = fkey] @"" 
```

## 5. Dotted as path, unwind: { "$unwind" : { "path" : "$a.b", "preserveNullAndEmptyArrays" : true } }

### Pipeline
```json
[
	{
		"$lookup" : {
			"from" : "foreignDotted",
			"localField" : "lkey",
			"foreignField" : "fkey",
			"as" : "a.b"
		}
	},
	{
		"$unwind" : {
			"path" : "$a.b",
			"preserveNullAndEmptyArrays" : true
		}
	}
]
```
### Options
```json
{ "allowDiskUse" : true }
```
### Results
```text
{ "_id" : 0, "lkey" : 1, "a" : { "x" : 10, "b" : { "_id" : 10, "fkey" : 1, "c" : 100 } } }
{ "_id" : 0, "lkey" : 1, "a" : { "x" : 10, "b" : { "_id" : 11, "fkey" : 1, "c" : 200 } } }
{ "_id" : 1, "lkey" : 2, "a" : { } }
{ "_id" : 2, "lkey" : [ 1, 3 ], "a" : { "b" : { "_id" : 10, "fkey" : 1, "c" : 100 } } }
{ "_id" : 2, "lkey" : [ 1, 3 ], "a" : { "b" : { "_id" : 11, "fkey" : 1, "c" : 200 } } }
{ "_id" : 2, "lkey" : [ 1, 3 ], "a" : { "b" : { "_id" : 12, "fkey" : 3, "c" : 300 } } }
{ "_id" : 3, "lkey" : 3, "a" : { "b" : { "_id" : 12, "fkey" : 3, "c" : 300 } } }
{ "_id" : 4, "a" : { "x" : 1, "b" : { "_id" : 13, "fkey" : null, "c" : 400 } } }
{ "_id" : 5, "lkey" : null, "a" : { "b" : { "_id" : 13, "fkey" : null, "c" : 400 } } }
```
### Plan
Engine: SBE
Strategy: HashJoin
Slots: $$RESULT=s10 env: {  }
```text
[3] project [s10 = makeBsonObj(MakeObjSpec([a = MakeObj([b = Set(0)], Open, NewObj, 0)], Open, NewObj, 0), s1, s9)] 
[3] hash_lookup_unwind left s9 
    outer s7 
        [3] project [s7 = arrayToSet(
            if (isArray(s3) ?: false) 
            then 
                if isArrayEmpty(s3) 
                then [null] 
                else s3 
            
            else newArray((s3 ?: null)) 
       )] 
        [1] scan generic [s1 = record, s2 = recordId] [s3 = lkey] @"" 
    inner s8 s4 
        [3] project [s8 = arrayToSet(
            let [
                l3.0 = (s6 ?: null) 
            ] 
            in 
                if isArray(l3.0) 
                then concatArrays(l3.0, newArray(l3.0)) 
                else newArray(move(l3.0)) 
       )] 
        [2] scan generic [s4 = record, s5 = recordId] [s6 = fkey] @"" 
```

### Pipeline
```json
[
	{
		"$lookup" : {
			"from" : "foreignDotted",
			"localField" : "lkey",
			"foreignField" : "fkey",
			"as" : "a.b"
		}
	},
	{
		"$unwind" : {
			"path" : "$a.b",
			"preserveNullAndEmptyArrays" : true
		}
	},
	{
		"$match" : {
			"a" : {
				"$exists" : true
			}
		}
	}
]
```
### Options
```json
{ "allowDiskUse" : true }
```
### Results
```text
{ "_id" : 0, "lkey" : 1, "a" : { "x" : 10, "b" : { "_id" : 10, "fkey" : 1, "c" : 100 } } }
{ "_id" : 0, "lkey" : 1, "a" : { "x" : 10, "b" : { "_id" : 11, "fkey" : 1, "c" : 200 } } }
{ "_id" : 1, "lkey" : 2, "a" : { } }
{ "_id" : 2, "lkey" : [ 1, 3 ], "a" : { "b" : { "_id" : 10, "fkey" : 1, "c" : 100 } } }
{ "_id" : 2, "lkey" : [ 1, 3 ], "a" : { "b" : { "_id" : 11, "fkey" : 1, "c" : 200 } } }
{ "_id" : 2, "lkey" : [ 1, 3 ], "a" : { "b" : { "_id" : 12, "fkey" : 3, "c" : 300 } } }
{ "_id" : 3, "lkey" : 3, "a" : { "b" : { "_id" : 12, "fkey" : 3, "c" : 300 } } }
{ "_id" : 4, "a" : { "x" : 1, "b" : { "_id" : 13, "fkey" : null, "c" : 400 } } }
{ "_id" : 5, "lkey" : null, "a" : { "b" : { "_id" : 13, "fkey" : null, "c" : 400 } } }
```
### Plan
Engine: SBE
Strategy: HashJoin
Slots: $$RESULT=s12 env: {  }
```text
[4] filter {exists(s13)} 
[3] project [s13 = getField(s12, "a")] 
[3] project [s12 = makeObj(MakeObjSpec([a = MakeObj([b = Set(0)], Open, NewObj, 0)], Open, NewObj, 0), s1, s11)] 
[3] hash_lookup_unwind left s11 
    outer s9 
        [3] project [s9 = arrayToSet(
            if (isArray(s4) ?: false) 
            then 
                if isArrayEmpty(s4) 
                then [null] 
                else s4 
            
            else newArray((s4 ?: null)) 
       )] 
        [1] scan generic [s1 = record, s2 = recordId] [s3 = a, s4 = lkey] @"" 
    inner s10 s5 
        [3] project [s10 = arrayToSet(
            let [
                l3.0 = (s8 ?: null) 
            ] 
            in 
                if isArray(l3.0) 
                then concatArrays(l3.0, newArray(l3.0)) 
                else newArray(move(l3.0)) 
       )] 
        [2] scan generic [s5 = record, s6 = recordId] [s7 = a, s8 = fkey] @"" 
```

### Pipeline
```json
[
	{
		"$lookup" : {
			"from" : "foreignDotted",
			"localField" : "lkey",
			"foreignField" : "fkey",
			"as" : "a.b"
		}
	},
	{
		"$unwind" : {
			"path" : "$a.b",
			"preserveNullAndEmptyArrays" : true
		}
	},
	{
		"$match" : {
			"a.b" : {
				"$exists" : true
			}
		}
	}
]
```
### Options
```json
{ "allowDiskUse" : true }
```
### Results
```text
{ "_id" : 0, "lkey" : 1, "a" : { "x" : 10, "b" : { "_id" : 10, "fkey" : 1, "c" : 100 } } }
{ "_id" : 0, "lkey" : 1, "a" : { "x" : 10, "b" : { "_id" : 11, "fkey" : 1, "c" : 200 } } }
{ "_id" : 2, "lkey" : [ 1, 3 ], "a" : { "b" : { "_id" : 10, "fkey" : 1, "c" : 100 } } }
{ "_id" : 2, "lkey" : [ 1, 3 ], "a" : { "b" : { "_id" : 11, "fkey" : 1, "c" : 200 } } }
{ "_id" : 2, "lkey" : [ 1, 3 ], "a" : { "b" : { "_id" : 12, "fkey" : 3, "c" : 300 } } }
{ "_id" : 3, "lkey" : 3, "a" : { "b" : { "_id" : 12, "fkey" : 3, "c" : 300 } } }
{ "_id" : 4, "a" : { "x" : 1, "b" : { "_id" : 13, "fkey" : null, "c" : 400 } } }
{ "_id" : 5, "lkey" : null, "a" : { "b" : { "_id" : 13, "fkey" : null, "c" : 400 } } }
```
### Plan
Engine: SBE
Strategy: HashJoin
Slots: $$RESULT=s12 env: {  }
```text
[4] filter {traverseF(s13, lambda(l6.0) { exists(getField(move(l6.0), "b")) }, false)} 
[3] project [s13 = getField(s12, "a")] 
[3] project [s12 = makeObj(MakeObjSpec([a = MakeObj([b = Set(0)], Open, NewObj, 0)], Open, NewObj, 0), s1, s11)] 
[3] hash_lookup_unwind left s11 
    outer s9 
        [3] project [s9 = arrayToSet(
            if (isArray(s4) ?: false) 
            then 
                if isArrayEmpty(s4) 
                then [null] 
                else s4 
            
            else newArray((s4 ?: null)) 
       )] 
        [1] scan generic [s1 = record, s2 = recordId] [s3 = a, s4 = lkey] @"" 
    inner s10 s5 
        [3] project [s10 = arrayToSet(
            let [
                l3.0 = (s8 ?: null) 
            ] 
            in 
                if isArray(l3.0) 
                then concatArrays(l3.0, newArray(l3.0)) 
                else newArray(move(l3.0)) 
       )] 
        [2] scan generic [s5 = record, s6 = recordId] [s7 = a, s8 = fkey] @"" 
```

### Pipeline
```json
[
	{
		"$lookup" : {
			"from" : "foreignDotted",
			"localField" : "lkey",
			"foreignField" : "fkey",
			"as" : "a.b"
		}
	},
	{
		"$unwind" : {
			"path" : "$a.b",
			"preserveNullAndEmptyArrays" : true
		}
	},
	{
		"$match" : {
			"a.b.c" : {
				"$gt" : 150
			}
		}
	}
]
```
### Options
```json
{ "allowDiskUse" : true }
```
### Results
```text
{ "_id" : 0, "lkey" : 1, "a" : { "x" : 10, "b" : { "_id" : 11, "fkey" : 1, "c" : 200 } } }
{ "_id" : 2, "lkey" : [ 1, 3 ], "a" : { "b" : { "_id" : 11, "fkey" : 1, "c" : 200 } } }
{ "_id" : 2, "lkey" : [ 1, 3 ], "a" : { "b" : { "_id" : 12, "fkey" : 3, "c" : 300 } } }
{ "_id" : 3, "lkey" : 3, "a" : { "b" : { "_id" : 12, "fkey" : 3, "c" : 300 } } }
{ "_id" : 4, "a" : { "x" : 1, "b" : { "_id" : 13, "fkey" : null, "c" : 400 } } }
{ "_id" : 5, "lkey" : null, "a" : { "b" : { "_id" : 13, "fkey" : null, "c" : 400 } } }
```
### Plan
Engine: SBE
Strategy: HashJoin
Slots: $$RESULT=s12 env: {  }
```text
[4] filter {traverseF(s13, lambda(l7.0) { traverseF(getField(move(l7.0), "b"), lambda(l8.0) { traverseF(getField(move(l8.0), "c"), lambda(l9.0) { ((move(l9.0) > 150L) ?: false) }, false) }, false) }, false)} 
[3] project [s13 = getField(s12, "a")] 
[3] project [s12 = makeObj(MakeObjSpec([a = MakeObj([b = Set(0)], Open, NewObj, 0)], Open, NewObj, 0), s1, s11)] 
[3] hash_lookup_unwind left s11 
    outer s9 
        [3] project [s9 = arrayToSet(
            if (isArray(s4) ?: false) 
            then 
                if isArrayEmpty(s4) 
                then [null] 
                else s4 
            
            else newArray((s4 ?: null)) 
       )] 
        [1] scan generic [s1 = record, s2 = recordId] [s3 = a, s4 = lkey] @"" 
    inner s10 s5 
        [3] project [s10 = arrayToSet(
            let [
                l3.0 = (s8 ?: null) 
            ] 
            in 
                if isArray(l3.0) 
                then concatArrays(l3.0, newArray(l3.0)) 
                else newArray(move(l3.0)) 
       )] 
        [2] scan generic [s5 = record, s6 = recordId] [s7 = a, s8 = fkey] @"" 
```

### Pipeline
```json
[
	{
		"$lookup" : {
			"from" : "foreignDotted",
			"localField" : "lkey",
			"foreignField" : "fkey",
			"as" : "a.b"
		}
	},
	{
		"$unwind" : {
			"path" : "$a.b",
			"preserveNullAndEmptyArrays" : true
		}
	},
	{
		"$match" : {
			"a" : {
				"$exists" : true
			},
			"a.b.c" : {
				"$gt" : 150
			}
		}
	}
]
```
### Options
```json
{ "allowDiskUse" : true }
```
### Results
```text
{ "_id" : 0, "lkey" : 1, "a" : { "x" : 10, "b" : { "_id" : 11, "fkey" : 1, "c" : 200 } } }
{ "_id" : 2, "lkey" : [ 1, 3 ], "a" : { "b" : { "_id" : 11, "fkey" : 1, "c" : 200 } } }
{ "_id" : 2, "lkey" : [ 1, 3 ], "a" : { "b" : { "_id" : 12, "fkey" : 3, "c" : 300 } } }
{ "_id" : 3, "lkey" : 3, "a" : { "b" : { "_id" : 12, "fkey" : 3, "c" : 300 } } }
{ "_id" : 4, "a" : { "x" : 1, "b" : { "_id" : 13, "fkey" : null, "c" : 400 } } }
{ "_id" : 5, "lkey" : null, "a" : { "b" : { "_id" : 13, "fkey" : null, "c" : 400 } } }
```
### Plan
Engine: SBE
Strategy: HashJoin
Slots: $$RESULT=s12 env: {  }
```text
[4] filter {(exists(s13) && traverseF(s13, lambda(l8.0) { traverseF(getField(move(l8.0), "b"), lambda(l9.0) { traverseF(getField(move(l9.0), "c"), lambda(l10.0) { ((move(l10.0) > 150L) ?: false) }, false) }, false) }, false))} 
[3] project [s13 = getField(s12, "a")] 
[3] project [s12 = makeObj(MakeObjSpec([a = MakeObj([b = Set(0)], Open, NewObj, 0)], Open, NewObj, 0), s1, s11)] 
[3] hash_lookup_unwind left s11 
    outer s9 
        [3] project [s9 = arrayToSet(
            if (isArray(s4) ?: false) 
            then 
                if isArrayEmpty(s4) 
                then [null] 
                else s4 
            
            else newArray((s4 ?: null)) 
       )] 
        [1] scan generic [s1 = record, s2 = recordId] [s3 = a, s4 = lkey] @"" 
    inner s10 s5 
        [3] project [s10 = arrayToSet(
            let [
                l3.0 = (s8 ?: null) 
            ] 
            in 
                if isArray(l3.0) 
                then concatArrays(l3.0, newArray(l3.0)) 
                else newArray(move(l3.0)) 
       )] 
        [2] scan generic [s5 = record, s6 = recordId] [s7 = a, s8 = fkey] @"" 
```

## 6. Dotted lookup: { "$lookup" : { "from" : "dottedForeign", "localField" : "a.b", "foreignField" : "fkey", "as" : "joined" } }

## 7. Unwind: { "$unwind" : "$joined" }

### Pipeline
```json
[
	{
		"$lookup" : {
			"from" : "dottedForeign",
			"localField" : "a.b",
			"foreignField" : "fkey",
			"as" : "joined"
		}
	},
	{
		"$unwind" : "$joined"
	},
	{
		"$addFields" : {
			"computed" : {
				"$add" : [
					"$lkey",
					"$joined.b"
				]
			}
		}
	}
]
```
### Options
```json
{ "allowDiskUse" : true }
```
### Results
```text
{ "_id" : 0, "lkey" : 1, "a" : { "b" : 1 }, "joined" : { "_id" : 10, "fkey" : 1, "b" : 10, "a" : { "b" : 1 } }, "computed" : 11 }
{ "_id" : 1, "lkey" : 2, "a" : { "b" : [ 1, 3 ] }, "joined" : { "_id" : 10, "fkey" : 1, "b" : 10, "a" : { "b" : 1 } }, "computed" : 12 }
{ "_id" : 1, "lkey" : 2, "a" : { "b" : [ 1, 3 ] }, "joined" : { "_id" : 15, "fkey" : 3, "b" : 60 }, "computed" : 62 }
{ "_id" : 2, "lkey" : 3, "a" : { "b" : null }, "joined" : { "_id" : 13, "b" : 40, "a" : { "b" : null } }, "computed" : 43 }
{ "_id" : 2, "lkey" : 3, "a" : { "b" : null }, "joined" : { "_id" : 16, "fkey" : null, "b" : 70, "a" : { "b" : 3 } }, "computed" : 73 }
{ "_id" : 3, "lkey" : 4, "a" : { "c" : 1 }, "joined" : { "_id" : 13, "b" : 40, "a" : { "b" : null } }, "computed" : 44 }
{ "_id" : 3, "lkey" : 4, "a" : { "c" : 1 }, "joined" : { "_id" : 16, "fkey" : null, "b" : 70, "a" : { "b" : 3 } }, "computed" : 74 }
{ "_id" : 4, "lkey" : null, "joined" : { "_id" : 13, "b" : 40, "a" : { "b" : null } }, "computed" : null }
{ "_id" : 4, "lkey" : null, "joined" : { "_id" : 16, "fkey" : null, "b" : 70, "a" : { "b" : 3 } }, "computed" : null }
```
### Plan
Engine: SBE
Strategy: HashJoin
Slots: $$RESULT=s15 env: {  }
```text
[4] project [s15 = makeBsonObj(MakeObjSpec([joined = Set(0), computed = Set(1)], Open, NewObj, 0), s1, s12, s14)] 
[4] project [s14 = 
    if (isNullish(s4) || isNullish(s13)) 
    then null 
    elif ((
        if isNumber(s4) 
        then 0 
        elif isDate(s4) 
        then 1 
        else fail(7157723, "only numbers and dates are allowed in an $add expression") 
    + 
        if isNumber(s13) 
        then 0 
        elif isDate(s13) 
        then 1 
        else fail(7157723, "only numbers and dates are allowed in an $add expression") 
   ) > 1) 
    then fail(7157722, "only one date allowed in an $add expression") 
    else (s4 + s13) 
] 
[4] extract_field_paths inputs[s4 = Get(lkey)/Id, s12 = Get(joined)/Id] outputs[s13 = Get(joined)/Traverse/Get(b)/Id] 
[3] hash_lookup_unwind inner s12 
    outer s10 
        [3] project [s10 = 
            let [
                l4.0 = 
                    let [
                        l5.0 = traverseP(s3, lambda(l6.0) { getField(move(l6.0), "b") }, 1) 
                    ] 
                    in 
                        if isArray(s3) 
                        then unwindArray(move(l5.0)) 
                        else makeOwn(move(l5.0)) 
            ] 
            in 
                if (isArray(l4.0) ?: false) 
                then 
                    if isArrayEmpty(l4.0) 
                    then [null] 
                    else makeOwn(move(l4.0)) 
                
                else newArray((makeOwn(move(l4.0)) ?: null)) 
       ] 
        [1] scan generic [s1 = record, s2 = recordId] [s3 = a, s4 = lkey] @"" 
    inner s11 s5 
        [3] project [s11 = arrayToSet(
            let [
                l8.0 = (s7 ?: null) 
            ] 
            in 
                if isArray(l8.0) 
                then concatArrays(l8.0, newArray(l8.0)) 
                else newArray(move(l8.0)) 
       )] 
        [2] scan generic [s5 = record, s6 = recordId] [s7 = fkey, s8 = joined, s9 = lkey] @"" 
```

### Pipeline
```json
[
	{
		"$lookup" : {
			"from" : "dottedForeign",
			"localField" : "a.b",
			"foreignField" : "fkey",
			"as" : "joined"
		}
	},
	{
		"$unwind" : "$joined"
	},
	{
		"$project" : {
			"_id" : 0,
			"lkey" : 1,
			"joined" : 1
		}
	}
]
```
### Options
```json
{ "allowDiskUse" : true }
```
### Results
```text
{ "lkey" : 1, "joined" : { "_id" : 10, "fkey" : 1, "b" : 10, "a" : { "b" : 1 } } }
{ "lkey" : 2, "joined" : { "_id" : 10, "fkey" : 1, "b" : 10, "a" : { "b" : 1 } } }
{ "lkey" : 2, "joined" : { "_id" : 15, "fkey" : 3, "b" : 60 } }
{ "lkey" : 3, "joined" : { "_id" : 13, "b" : 40, "a" : { "b" : null } } }
{ "lkey" : 3, "joined" : { "_id" : 16, "fkey" : null, "b" : 70, "a" : { "b" : 3 } } }
{ "lkey" : 4, "joined" : { "_id" : 13, "b" : 40, "a" : { "b" : null } } }
{ "lkey" : 4, "joined" : { "_id" : 16, "fkey" : null, "b" : 70, "a" : { "b" : 3 } } }
{ "lkey" : null, "joined" : { "_id" : 13, "b" : 40, "a" : { "b" : null } } }
{ "lkey" : null, "joined" : { "_id" : 16, "fkey" : null, "b" : 70, "a" : { "b" : 3 } } }
```
### Plan
Engine: classic

## 8. Unwind: { "$unwind" : { "path" : "$joined", "preserveNullAndEmptyArrays" : true } }

### Pipeline
```json
[
	{
		"$lookup" : {
			"from" : "dottedForeign",
			"localField" : "a.b",
			"foreignField" : "fkey",
			"as" : "joined"
		}
	},
	{
		"$unwind" : {
			"path" : "$joined",
			"preserveNullAndEmptyArrays" : true
		}
	},
	{
		"$addFields" : {
			"computed" : {
				"$add" : [
					"$lkey",
					"$joined.b"
				]
			}
		}
	}
]
```
### Options
```json
{ "allowDiskUse" : true }
```
### Results
```text
{ "_id" : 0, "lkey" : 1, "a" : { "b" : 1 }, "joined" : { "_id" : 10, "fkey" : 1, "b" : 10, "a" : { "b" : 1 } }, "computed" : 11 }
{ "_id" : 1, "lkey" : 2, "a" : { "b" : [ 1, 3 ] }, "joined" : { "_id" : 10, "fkey" : 1, "b" : 10, "a" : { "b" : 1 } }, "computed" : 12 }
{ "_id" : 1, "lkey" : 2, "a" : { "b" : [ 1, 3 ] }, "joined" : { "_id" : 15, "fkey" : 3, "b" : 60 }, "computed" : 62 }
{ "_id" : 2, "lkey" : 3, "a" : { "b" : null }, "joined" : { "_id" : 13, "b" : 40, "a" : { "b" : null } }, "computed" : 43 }
{ "_id" : 2, "lkey" : 3, "a" : { "b" : null }, "joined" : { "_id" : 16, "fkey" : null, "b" : 70, "a" : { "b" : 3 } }, "computed" : 73 }
{ "_id" : 3, "lkey" : 4, "a" : { "c" : 1 }, "joined" : { "_id" : 13, "b" : 40, "a" : { "b" : null } }, "computed" : 44 }
{ "_id" : 3, "lkey" : 4, "a" : { "c" : 1 }, "joined" : { "_id" : 16, "fkey" : null, "b" : 70, "a" : { "b" : 3 } }, "computed" : 74 }
{ "_id" : 4, "lkey" : null, "joined" : { "_id" : 13, "b" : 40, "a" : { "b" : null } }, "computed" : null }
{ "_id" : 4, "lkey" : null, "joined" : { "_id" : 16, "fkey" : null, "b" : 70, "a" : { "b" : 3 } }, "computed" : null }
```
### Plan
Engine: SBE
Strategy: HashJoin
Slots: $$RESULT=s19 env: {  }
```text
[4] project [s19 = makeBsonObj(MakeObjSpec([computed = Set(0)], Open, NewObj, 0), s14, s18)] 
[4] project [s18 = 
    if (isNullish(s16) || isNullish(s17)) 
    then null 
    elif ((
        if isNumber(s16) 
        then 0 
        elif isDate(s16) 
        then 1 
        else fail(7157723, "only numbers and dates are allowed in an $add expression") 
    + 
        if isNumber(s17) 
        then 0 
        elif isDate(s17) 
        then 1 
        else fail(7157723, "only numbers and dates are allowed in an $add expression") 
   ) > 1) 
    then fail(7157722, "only one date allowed in an $add expression") 
    else (s16 + s17) 
] 
[4] extract_field_paths inputs[s15 = Get(joined)/Id, s16 = Get(lkey)/Id] outputs[s17 = Get(joined)/Traverse/Get(b)/Id] 
[3] project [s15 = getField(s14, "joined"), s16 = getField(s14, "lkey")] 
[3] project [s14 = makeObj(MakeObjSpec([joined = Set(0)], Open, NewObj, 0), s1, s13)] 
[3] hash_lookup_unwind left s13 
    outer s11 
        [3] project [s11 = 
            let [
                l4.0 = 
                    let [
                        l5.0 = traverseP(s3, lambda(l6.0) { getField(move(l6.0), "b") }, 1) 
                    ] 
                    in 
                        if isArray(s3) 
                        then unwindArray(move(l5.0)) 
                        else makeOwn(move(l5.0)) 
            ] 
            in 
                if (isArray(l4.0) ?: false) 
                then 
                    if isArrayEmpty(l4.0) 
                    then [null] 
                    else makeOwn(move(l4.0)) 
                
                else newArray((makeOwn(move(l4.0)) ?: null)) 
       ] 
        [1] scan generic [s1 = record, s2 = recordId] [s3 = a, s4 = joined, s5 = lkey] @"" 
    inner s12 s6 
        [3] project [s12 = arrayToSet(
            let [
                l8.0 = (s8 ?: null) 
            ] 
            in 
                if isArray(l8.0) 
                then concatArrays(l8.0, newArray(l8.0)) 
                else newArray(move(l8.0)) 
       )] 
        [2] scan generic [s6 = record, s7 = recordId] [s8 = fkey, s9 = joined, s10 = lkey] @"" 
```

### Pipeline
```json
[
	{
		"$lookup" : {
			"from" : "dottedForeign",
			"localField" : "a.b",
			"foreignField" : "fkey",
			"as" : "joined"
		}
	},
	{
		"$unwind" : {
			"path" : "$joined",
			"preserveNullAndEmptyArrays" : true
		}
	},
	{
		"$project" : {
			"_id" : 0,
			"lkey" : 1,
			"joined" : 1
		}
	}
]
```
### Options
```json
{ "allowDiskUse" : true }
```
### Results
```text
{ "lkey" : 1, "joined" : { "_id" : 10, "fkey" : 1, "b" : 10, "a" : { "b" : 1 } } }
{ "lkey" : 2, "joined" : { "_id" : 10, "fkey" : 1, "b" : 10, "a" : { "b" : 1 } } }
{ "lkey" : 2, "joined" : { "_id" : 15, "fkey" : 3, "b" : 60 } }
{ "lkey" : 3, "joined" : { "_id" : 13, "b" : 40, "a" : { "b" : null } } }
{ "lkey" : 3, "joined" : { "_id" : 16, "fkey" : null, "b" : 70, "a" : { "b" : 3 } } }
{ "lkey" : 4, "joined" : { "_id" : 13, "b" : 40, "a" : { "b" : null } } }
{ "lkey" : 4, "joined" : { "_id" : 16, "fkey" : null, "b" : 70, "a" : { "b" : 3 } } }
{ "lkey" : null, "joined" : { "_id" : 13, "b" : 40, "a" : { "b" : null } } }
{ "lkey" : null, "joined" : { "_id" : 16, "fkey" : null, "b" : 70, "a" : { "b" : 3 } } }
```
### Plan
Engine: classic

## 9. Unwind: { "$unwind" : { "path" : "$joined", "includeArrayIndex" : "idx" } }

### Pipeline
```json
[
	{
		"$lookup" : {
			"from" : "dottedForeign",
			"localField" : "a.b",
			"foreignField" : "fkey",
			"as" : "joined"
		}
	},
	{
		"$unwind" : {
			"path" : "$joined",
			"includeArrayIndex" : "idx"
		}
	},
	{
		"$addFields" : {
			"computed" : {
				"$add" : [
					"$lkey",
					"$joined.b"
				]
			}
		}
	}
]
```
### Options
```json
{ "allowDiskUse" : true }
```
### Results
```text
{ "_id" : 0, "lkey" : 1, "a" : { "b" : 1 }, "joined" : { "_id" : 10, "fkey" : 1, "b" : 10, "a" : { "b" : 1 } }, "idx" : NumberLong(0), "computed" : 11 }
{ "_id" : 1, "lkey" : 2, "a" : { "b" : [ 1, 3 ] }, "joined" : { "_id" : 10, "fkey" : 1, "b" : 10, "a" : { "b" : 1 } }, "idx" : NumberLong(0), "computed" : 12 }
{ "_id" : 1, "lkey" : 2, "a" : { "b" : [ 1, 3 ] }, "joined" : { "_id" : 15, "fkey" : 3, "b" : 60 }, "idx" : NumberLong(1), "computed" : 62 }
{ "_id" : 2, "lkey" : 3, "a" : { "b" : null }, "joined" : { "_id" : 13, "b" : 40, "a" : { "b" : null } }, "idx" : NumberLong(0), "computed" : 43 }
{ "_id" : 2, "lkey" : 3, "a" : { "b" : null }, "joined" : { "_id" : 16, "fkey" : null, "b" : 70, "a" : { "b" : 3 } }, "idx" : NumberLong(1), "computed" : 73 }
{ "_id" : 3, "lkey" : 4, "a" : { "c" : 1 }, "joined" : { "_id" : 13, "b" : 40, "a" : { "b" : null } }, "idx" : NumberLong(0), "computed" : 44 }
{ "_id" : 3, "lkey" : 4, "a" : { "c" : 1 }, "joined" : { "_id" : 16, "fkey" : null, "b" : 70, "a" : { "b" : 3 } }, "idx" : NumberLong(1), "computed" : 74 }
{ "_id" : 4, "lkey" : null, "joined" : { "_id" : 13, "b" : 40, "a" : { "b" : null } }, "idx" : NumberLong(0), "computed" : null }
{ "_id" : 4, "lkey" : null, "joined" : { "_id" : 16, "fkey" : null, "b" : 70, "a" : { "b" : 3 } }, "idx" : NumberLong(1), "computed" : null }
```
### Plan
Engine: SBE
Strategy: HashJoin
Slots: $$RESULT=s20 env: {  }
```text
[4] project [s20 = makeBsonObj(MakeObjSpec([computed = Set(0)], Open, NewObj, 0), s15, s19)] 
[4] project [s19 = 
    if (isNullish(s17) || isNullish(s18)) 
    then null 
    elif ((
        if isNumber(s17) 
        then 0 
        elif isDate(s17) 
        then 1 
        else fail(7157723, "only numbers and dates are allowed in an $add expression") 
    + 
        if isNumber(s18) 
        then 0 
        elif isDate(s18) 
        then 1 
        else fail(7157723, "only numbers and dates are allowed in an $add expression") 
   ) > 1) 
    then fail(7157722, "only one date allowed in an $add expression") 
    else (s17 + s18) 
] 
[4] extract_field_paths inputs[s16 = Get(joined)/Id, s17 = Get(lkey)/Id] outputs[s18 = Get(joined)/Traverse/Get(b)/Id] 
[3] project [s16 = getField(s15, "joined"), s17 = getField(s15, "lkey")] 
[3] project [s15 = makeObj(MakeObjSpec([joined = Set(0), idx = Set(1)], Open, NewObj, 0), s1, s14, s6)] 
[3] hash_lookup_unwind inner s14 s6 
    outer s12 
        [3] project [s12 = 
            let [
                l4.0 = 
                    let [
                        l5.0 = traverseP(s3, lambda(l6.0) { getField(move(l6.0), "b") }, 1) 
                    ] 
                    in 
                        if isArray(s3) 
                        then unwindArray(move(l5.0)) 
                        else makeOwn(move(l5.0)) 
            ] 
            in 
                if (isArray(l4.0) ?: false) 
                then 
                    if isArrayEmpty(l4.0) 
                    then [null] 
                    else makeOwn(move(l4.0)) 
                
                else newArray((makeOwn(move(l4.0)) ?: null)) 
       ] 
        [1] scan generic [s1 = record, s2 = recordId] [s3 = a, s4 = joined, s5 = lkey] @"" 
    inner s13 s7 
        [3] project [s13 = arrayToSet(
            let [
                l8.0 = (s9 ?: null) 
            ] 
            in 
                if isArray(l8.0) 
                then concatArrays(l8.0, newArray(l8.0)) 
                else newArray(move(l8.0)) 
       )] 
        [2] scan generic [s7 = record, s8 = recordId] [s9 = fkey, s10 = joined, s11 = lkey] @"" 
```

### Pipeline
```json
[
	{
		"$lookup" : {
			"from" : "dottedForeign",
			"localField" : "a.b",
			"foreignField" : "fkey",
			"as" : "joined"
		}
	},
	{
		"$unwind" : {
			"path" : "$joined",
			"includeArrayIndex" : "idx"
		}
	},
	{
		"$project" : {
			"_id" : 0,
			"lkey" : 1,
			"joined" : 1
		}
	}
]
```
### Options
```json
{ "allowDiskUse" : true }
```
### Results
```text
{ "lkey" : 1, "joined" : { "_id" : 10, "fkey" : 1, "b" : 10, "a" : { "b" : 1 } } }
{ "lkey" : 2, "joined" : { "_id" : 10, "fkey" : 1, "b" : 10, "a" : { "b" : 1 } } }
{ "lkey" : 2, "joined" : { "_id" : 15, "fkey" : 3, "b" : 60 } }
{ "lkey" : 3, "joined" : { "_id" : 13, "b" : 40, "a" : { "b" : null } } }
{ "lkey" : 3, "joined" : { "_id" : 16, "fkey" : null, "b" : 70, "a" : { "b" : 3 } } }
{ "lkey" : 4, "joined" : { "_id" : 13, "b" : 40, "a" : { "b" : null } } }
{ "lkey" : 4, "joined" : { "_id" : 16, "fkey" : null, "b" : 70, "a" : { "b" : 3 } } }
{ "lkey" : null, "joined" : { "_id" : 13, "b" : 40, "a" : { "b" : null } } }
{ "lkey" : null, "joined" : { "_id" : 16, "fkey" : null, "b" : 70, "a" : { "b" : 3 } } }
```
### Plan
Engine: classic

## 10. Dotted lookup: { "$lookup" : { "from" : "dottedForeign", "localField" : "lkey", "foreignField" : "a.b", "as" : "joined" } }

## 11. Unwind: { "$unwind" : "$joined" }

### Pipeline
```json
[
	{
		"$lookup" : {
			"from" : "dottedForeign",
			"localField" : "lkey",
			"foreignField" : "a.b",
			"as" : "joined"
		}
	},
	{
		"$unwind" : "$joined"
	},
	{
		"$addFields" : {
			"computed" : {
				"$add" : [
					"$lkey",
					"$joined.b"
				]
			}
		}
	}
]
```
### Options
```json
{ "allowDiskUse" : true }
```
### Results
```text
{ "_id" : 0, "lkey" : 1, "a" : { "b" : 1 }, "joined" : { "_id" : 10, "fkey" : 1, "b" : 10, "a" : { "b" : 1 } }, "computed" : 11 }
{ "_id" : 0, "lkey" : 1, "a" : { "b" : 1 }, "joined" : { "_id" : 11, "fkey" : 2, "b" : 20, "a" : { "b" : [ 1, 3 ] } }, "computed" : 21 }
{ "_id" : 1, "lkey" : 2, "a" : { "b" : [ 1, 3 ] }, "joined" : { "_id" : 12, "fkey" : [ 2, 4 ], "b" : 30, "a" : { "b" : 2 } }, "computed" : 32 }
{ "_id" : 2, "lkey" : 3, "a" : { "b" : null }, "joined" : { "_id" : 11, "fkey" : 2, "b" : 20, "a" : { "b" : [ 1, 3 ] } }, "computed" : 23 }
{ "_id" : 2, "lkey" : 3, "a" : { "b" : null }, "joined" : { "_id" : 16, "fkey" : null, "b" : 70, "a" : { "b" : 3 } }, "computed" : 73 }
{ "_id" : 4, "lkey" : null, "joined" : { "_id" : 13, "b" : 40, "a" : { "b" : null } }, "computed" : null }
{ "_id" : 4, "lkey" : null, "joined" : { "_id" : 14, "fkey" : 4, "b" : 50, "a" : { "c" : 1 } }, "computed" : null }
{ "_id" : 4, "lkey" : null, "joined" : { "_id" : 15, "fkey" : 3, "b" : 60 }, "computed" : null }
```
### Plan
Engine: SBE
Strategy: HashJoin
Slots: $$RESULT=s14 env: {  }
```text
[4] project [s14 = makeBsonObj(MakeObjSpec([joined = Set(0), computed = Set(1)], Open, NewObj, 0), s1, s11, s13)] 
[4] project [s13 = 
    if (isNullish(s3) || isNullish(s12)) 
    then null 
    elif ((
        if isNumber(s3) 
        then 0 
        elif isDate(s3) 
        then 1 
        else fail(7157723, "only numbers and dates are allowed in an $add expression") 
    + 
        if isNumber(s12) 
        then 0 
        elif isDate(s12) 
        then 1 
        else fail(7157723, "only numbers and dates are allowed in an $add expression") 
   ) > 1) 
    then fail(7157722, "only one date allowed in an $add expression") 
    else (s3 + s12) 
] 
[4] extract_field_paths inputs[s3 = Get(lkey)/Id, s11 = Get(joined)/Id] outputs[s12 = Get(joined)/Traverse/Get(b)/Id] 
[3] hash_lookup_unwind inner s11 
    outer s9 
        [3] project [s9 = 
            if (isArray(s3) ?: false) 
            then 
                if isArrayEmpty(s3) 
                then [null] 
                else s3 
            
            else newArray((s3 ?: null)) 
       ] 
        [1] scan generic [s1 = record, s2 = recordId] [s3 = lkey] @"" 
    inner s10 s4 
        [3] project [s10 = arrayToSet(
            let [
                l5.0 = (s6 ?: null) 
                l5.1 = traverseP(l5.0, lambda(l6.0 l6.1) { 
                    let [
                        l7.0 = 
                            if ((move(l6.1) == -1ll) || isObject(l6.0)) 
                            then (getField(move(l6.0), "b") ?: null) 
                            else Nothing 
                    ] 
                    in 
                        if isArray(l7.0) 
                        then concatArrays(l7.0, newArray(l7.0)) 
                        else newArray(move(l7.0)) 
                }, 1) 
            ] 
            in 
                if isArray(move(l5.0)) 
                then unwindArray(move(l5.1)) 
                else makeOwn(move(l5.1)) 
       )] 
        [2] scan generic [s4 = record, s5 = recordId] [s6 = a, s7 = joined, s8 = lkey] @"" 
```

### Pipeline
```json
[
	{
		"$lookup" : {
			"from" : "dottedForeign",
			"localField" : "lkey",
			"foreignField" : "a.b",
			"as" : "joined"
		}
	},
	{
		"$unwind" : "$joined"
	},
	{
		"$project" : {
			"_id" : 0,
			"lkey" : 1,
			"joined" : 1
		}
	}
]
```
### Options
```json
{ "allowDiskUse" : true }
```
### Results
```text
{ "lkey" : 1, "joined" : { "_id" : 10, "fkey" : 1, "b" : 10, "a" : { "b" : 1 } } }
{ "lkey" : 1, "joined" : { "_id" : 11, "fkey" : 2, "b" : 20, "a" : { "b" : [ 1, 3 ] } } }
{ "lkey" : 2, "joined" : { "_id" : 12, "fkey" : [ 2, 4 ], "b" : 30, "a" : { "b" : 2 } } }
{ "lkey" : 3, "joined" : { "_id" : 11, "fkey" : 2, "b" : 20, "a" : { "b" : [ 1, 3 ] } } }
{ "lkey" : 3, "joined" : { "_id" : 16, "fkey" : null, "b" : 70, "a" : { "b" : 3 } } }
{ "lkey" : null, "joined" : { "_id" : 13, "b" : 40, "a" : { "b" : null } } }
{ "lkey" : null, "joined" : { "_id" : 14, "fkey" : 4, "b" : 50, "a" : { "c" : 1 } } }
{ "lkey" : null, "joined" : { "_id" : 15, "fkey" : 3, "b" : 60 } }
```
### Plan
Engine: classic

## 12. Unwind: { "$unwind" : { "path" : "$joined", "preserveNullAndEmptyArrays" : true } }

### Pipeline
```json
[
	{
		"$lookup" : {
			"from" : "dottedForeign",
			"localField" : "lkey",
			"foreignField" : "a.b",
			"as" : "joined"
		}
	},
	{
		"$unwind" : {
			"path" : "$joined",
			"preserveNullAndEmptyArrays" : true
		}
	},
	{
		"$addFields" : {
			"computed" : {
				"$add" : [
					"$lkey",
					"$joined.b"
				]
			}
		}
	}
]
```
### Options
```json
{ "allowDiskUse" : true }
```
### Results
```text
{ "_id" : 0, "lkey" : 1, "a" : { "b" : 1 }, "joined" : { "_id" : 10, "fkey" : 1, "b" : 10, "a" : { "b" : 1 } }, "computed" : 11 }
{ "_id" : 0, "lkey" : 1, "a" : { "b" : 1 }, "joined" : { "_id" : 11, "fkey" : 2, "b" : 20, "a" : { "b" : [ 1, 3 ] } }, "computed" : 21 }
{ "_id" : 1, "lkey" : 2, "a" : { "b" : [ 1, 3 ] }, "joined" : { "_id" : 12, "fkey" : [ 2, 4 ], "b" : 30, "a" : { "b" : 2 } }, "computed" : 32 }
{ "_id" : 2, "lkey" : 3, "a" : { "b" : null }, "joined" : { "_id" : 11, "fkey" : 2, "b" : 20, "a" : { "b" : [ 1, 3 ] } }, "computed" : 23 }
{ "_id" : 2, "lkey" : 3, "a" : { "b" : null }, "joined" : { "_id" : 16, "fkey" : null, "b" : 70, "a" : { "b" : 3 } }, "computed" : 73 }
{ "_id" : 3, "lkey" : 4, "a" : { "c" : 1 }, "computed" : null }
{ "_id" : 4, "lkey" : null, "joined" : { "_id" : 13, "b" : 40, "a" : { "b" : null } }, "computed" : null }
{ "_id" : 4, "lkey" : null, "joined" : { "_id" : 14, "fkey" : 4, "b" : 50, "a" : { "c" : 1 } }, "computed" : null }
{ "_id" : 4, "lkey" : null, "joined" : { "_id" : 15, "fkey" : 3, "b" : 60 }, "computed" : null }
```
### Plan
Engine: SBE
Strategy: HashJoin
Slots: $$RESULT=s18 env: {  }
```text
[4] project [s18 = makeBsonObj(MakeObjSpec([computed = Set(0)], Open, NewObj, 0), s13, s17)] 
[4] project [s17 = 
    if (isNullish(s15) || isNullish(s16)) 
    then null 
    elif ((
        if isNumber(s15) 
        then 0 
        elif isDate(s15) 
        then 1 
        else fail(7157723, "only numbers and dates are allowed in an $add expression") 
    + 
        if isNumber(s16) 
        then 0 
        elif isDate(s16) 
        then 1 
        else fail(7157723, "only numbers and dates are allowed in an $add expression") 
   ) > 1) 
    then fail(7157722, "only one date allowed in an $add expression") 
    else (s15 + s16) 
] 
[4] extract_field_paths inputs[s14 = Get(joined)/Id, s15 = Get(lkey)/Id] outputs[s16 = Get(joined)/Traverse/Get(b)/Id] 
[3] project [s14 = getField(s13, "joined"), s15 = getField(s13, "lkey")] 
[3] project [s13 = makeObj(MakeObjSpec([joined = Set(0)], Open, NewObj, 0), s1, s12)] 
[3] hash_lookup_unwind left s12 
    outer s10 
        [3] project [s10 = 
            if (isArray(s4) ?: false) 
            then 
                if isArrayEmpty(s4) 
                then [null] 
                else s4 
            
            else newArray((s4 ?: null)) 
       ] 
        [1] scan generic [s1 = record, s2 = recordId] [s3 = joined, s4 = lkey] @"" 
    inner s11 s5 
        [3] project [s11 = arrayToSet(
            let [
                l5.0 = (s7 ?: null) 
                l5.1 = traverseP(l5.0, lambda(l6.0 l6.1) { 
                    let [
                        l7.0 = 
                            if ((move(l6.1) == -1ll) || isObject(l6.0)) 
                            then (getField(move(l6.0), "b") ?: null) 
                            else Nothing 
                    ] 
                    in 
                        if isArray(l7.0) 
                        then concatArrays(l7.0, newArray(l7.0)) 
                        else newArray(move(l7.0)) 
                }, 1) 
            ] 
            in 
                if isArray(move(l5.0)) 
                then unwindArray(move(l5.1)) 
                else makeOwn(move(l5.1)) 
       )] 
        [2] scan generic [s5 = record, s6 = recordId] [s7 = a, s8 = joined, s9 = lkey] @"" 
```

### Pipeline
```json
[
	{
		"$lookup" : {
			"from" : "dottedForeign",
			"localField" : "lkey",
			"foreignField" : "a.b",
			"as" : "joined"
		}
	},
	{
		"$unwind" : {
			"path" : "$joined",
			"preserveNullAndEmptyArrays" : true
		}
	},
	{
		"$project" : {
			"_id" : 0,
			"lkey" : 1,
			"joined" : 1
		}
	}
]
```
### Options
```json
{ "allowDiskUse" : true }
```
### Results
```text
{ "lkey" : 1, "joined" : { "_id" : 10, "fkey" : 1, "b" : 10, "a" : { "b" : 1 } } }
{ "lkey" : 1, "joined" : { "_id" : 11, "fkey" : 2, "b" : 20, "a" : { "b" : [ 1, 3 ] } } }
{ "lkey" : 2, "joined" : { "_id" : 12, "fkey" : [ 2, 4 ], "b" : 30, "a" : { "b" : 2 } } }
{ "lkey" : 3, "joined" : { "_id" : 11, "fkey" : 2, "b" : 20, "a" : { "b" : [ 1, 3 ] } } }
{ "lkey" : 3, "joined" : { "_id" : 16, "fkey" : null, "b" : 70, "a" : { "b" : 3 } } }
{ "lkey" : 4 }
{ "lkey" : null, "joined" : { "_id" : 13, "b" : 40, "a" : { "b" : null } } }
{ "lkey" : null, "joined" : { "_id" : 14, "fkey" : 4, "b" : 50, "a" : { "c" : 1 } } }
{ "lkey" : null, "joined" : { "_id" : 15, "fkey" : 3, "b" : 60 } }
```
### Plan
Engine: classic

## 13. Unwind: { "$unwind" : { "path" : "$joined", "includeArrayIndex" : "idx" } }

### Pipeline
```json
[
	{
		"$lookup" : {
			"from" : "dottedForeign",
			"localField" : "lkey",
			"foreignField" : "a.b",
			"as" : "joined"
		}
	},
	{
		"$unwind" : {
			"path" : "$joined",
			"includeArrayIndex" : "idx"
		}
	},
	{
		"$addFields" : {
			"computed" : {
				"$add" : [
					"$lkey",
					"$joined.b"
				]
			}
		}
	}
]
```
### Options
```json
{ "allowDiskUse" : true }
```
### Results
```text
{ "_id" : 0, "lkey" : 1, "a" : { "b" : 1 }, "joined" : { "_id" : 10, "fkey" : 1, "b" : 10, "a" : { "b" : 1 } }, "idx" : NumberLong(0), "computed" : 11 }
{ "_id" : 0, "lkey" : 1, "a" : { "b" : 1 }, "joined" : { "_id" : 11, "fkey" : 2, "b" : 20, "a" : { "b" : [ 1, 3 ] } }, "idx" : NumberLong(1), "computed" : 21 }
{ "_id" : 1, "lkey" : 2, "a" : { "b" : [ 1, 3 ] }, "joined" : { "_id" : 12, "fkey" : [ 2, 4 ], "b" : 30, "a" : { "b" : 2 } }, "idx" : NumberLong(0), "computed" : 32 }
{ "_id" : 2, "lkey" : 3, "a" : { "b" : null }, "joined" : { "_id" : 11, "fkey" : 2, "b" : 20, "a" : { "b" : [ 1, 3 ] } }, "idx" : NumberLong(0), "computed" : 23 }
{ "_id" : 2, "lkey" : 3, "a" : { "b" : null }, "joined" : { "_id" : 16, "fkey" : null, "b" : 70, "a" : { "b" : 3 } }, "idx" : NumberLong(1), "computed" : 73 }
{ "_id" : 4, "lkey" : null, "joined" : { "_id" : 13, "b" : 40, "a" : { "b" : null } }, "idx" : NumberLong(0), "computed" : null }
{ "_id" : 4, "lkey" : null, "joined" : { "_id" : 14, "fkey" : 4, "b" : 50, "a" : { "c" : 1 } }, "idx" : NumberLong(1), "computed" : null }
{ "_id" : 4, "lkey" : null, "joined" : { "_id" : 15, "fkey" : 3, "b" : 60 }, "idx" : NumberLong(2), "computed" : null }
```
### Plan
Engine: SBE
Strategy: HashJoin
Slots: $$RESULT=s19 env: {  }
```text
[4] project [s19 = makeBsonObj(MakeObjSpec([computed = Set(0)], Open, NewObj, 0), s14, s18)] 
[4] project [s18 = 
    if (isNullish(s16) || isNullish(s17)) 
    then null 
    elif ((
        if isNumber(s16) 
        then 0 
        elif isDate(s16) 
        then 1 
        else fail(7157723, "only numbers and dates are allowed in an $add expression") 
    + 
        if isNumber(s17) 
        then 0 
        elif isDate(s17) 
        then 1 
        else fail(7157723, "only numbers and dates are allowed in an $add expression") 
   ) > 1) 
    then fail(7157722, "only one date allowed in an $add expression") 
    else (s16 + s17) 
] 
[4] extract_field_paths inputs[s15 = Get(joined)/Id, s16 = Get(lkey)/Id] outputs[s17 = Get(joined)/Traverse/Get(b)/Id] 
[3] project [s15 = getField(s14, "joined"), s16 = getField(s14, "lkey")] 
[3] project [s14 = makeObj(MakeObjSpec([joined = Set(0), idx = Set(1)], Open, NewObj, 0), s1, s13, s5)] 
[3] hash_lookup_unwind inner s13 s5 
    outer s11 
        [3] project [s11 = 
            if (isArray(s4) ?: false) 
            then 
                if isArrayEmpty(s4) 
                then [null] 
                else s4 
            
            else newArray((s4 ?: null)) 
       ] 
        [1] scan generic [s1 = record, s2 = recordId] [s3 = joined, s4 = lkey] @"" 
    inner s12 s6 
        [3] project [s12 = arrayToSet(
            let [
                l5.0 = (s8 ?: null) 
                l5.1 = traverseP(l5.0, lambda(l6.0 l6.1) { 
                    let [
                        l7.0 = 
                            if ((move(l6.1) == -1ll) || isObject(l6.0)) 
                            then (getField(move(l6.0), "b") ?: null) 
                            else Nothing 
                    ] 
                    in 
                        if isArray(l7.0) 
                        then concatArrays(l7.0, newArray(l7.0)) 
                        else newArray(move(l7.0)) 
                }, 1) 
            ] 
            in 
                if isArray(move(l5.0)) 
                then unwindArray(move(l5.1)) 
                else makeOwn(move(l5.1)) 
       )] 
        [2] scan generic [s6 = record, s7 = recordId] [s8 = a, s9 = joined, s10 = lkey] @"" 
```

### Pipeline
```json
[
	{
		"$lookup" : {
			"from" : "dottedForeign",
			"localField" : "lkey",
			"foreignField" : "a.b",
			"as" : "joined"
		}
	},
	{
		"$unwind" : {
			"path" : "$joined",
			"includeArrayIndex" : "idx"
		}
	},
	{
		"$project" : {
			"_id" : 0,
			"lkey" : 1,
			"joined" : 1
		}
	}
]
```
### Options
```json
{ "allowDiskUse" : true }
```
### Results
```text
{ "lkey" : 1, "joined" : { "_id" : 10, "fkey" : 1, "b" : 10, "a" : { "b" : 1 } } }
{ "lkey" : 1, "joined" : { "_id" : 11, "fkey" : 2, "b" : 20, "a" : { "b" : [ 1, 3 ] } } }
{ "lkey" : 2, "joined" : { "_id" : 12, "fkey" : [ 2, 4 ], "b" : 30, "a" : { "b" : 2 } } }
{ "lkey" : 3, "joined" : { "_id" : 11, "fkey" : 2, "b" : 20, "a" : { "b" : [ 1, 3 ] } } }
{ "lkey" : 3, "joined" : { "_id" : 16, "fkey" : null, "b" : 70, "a" : { "b" : 3 } } }
{ "lkey" : null, "joined" : { "_id" : 13, "b" : 40, "a" : { "b" : null } } }
{ "lkey" : null, "joined" : { "_id" : 14, "fkey" : 4, "b" : 50, "a" : { "c" : 1 } } }
{ "lkey" : null, "joined" : { "_id" : 15, "fkey" : 3, "b" : 60 } }
```
### Plan
Engine: classic



[jsTest] ----
[jsTest] Done
[jsTest] ----

