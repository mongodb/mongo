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
Engine: classic

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
Engine: classic

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
Engine: classic

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
Engine: classic

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
Engine: classic



[jsTest] ----
[jsTest] Done
[jsTest] ----

