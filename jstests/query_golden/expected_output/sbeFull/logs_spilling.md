## 1. Sort with large memory limit
### Pipeline
```json
[ { "$sort" : { "a" : 1 } } ]
```
### Slow query spilling stats
```json
{ }
```

## 2. Sort with empty collection
### Pipeline
```json
[ { "$sort" : { "a" : 1 } } ]
```
### Slow query spilling stats
```json
{ }
```

## 3. Sort with spilling
### Pipeline
```json
[ { "$sort" : { "a" : 1 } } ]
```
### Slow query spilling stats
```json
{
	"sortSpilledBytes" : "X",
	"sortSpilledDataStorageSize" : "X",
	"sortSpilledRecords" : 8,
	"sortSpills" : 5,
	"usedDisk" : true
}
```

## 4. Multiple sorts
### Pipeline
```json
[
	{
		"$sort" : {
			"a" : 1
		}
	},
	{
		"$limit" : 3
	},
	{
		"$sort" : {
			"b" : 1
		}
	}
]
```
### Slow query spilling stats
```json
{
	"sortSpilledBytes" : "X",
	"sortSpilledDataStorageSize" : "X",
	"sortSpilledRecords" : 16,
	"sortSpills" : 10,
	"usedDisk" : true
}
```

## 5. Timeseries sort
### Pipeline
```json
[ { "$sort" : { "time" : 1 } } ]
```
### Slow query spilling stats
```json
{
	"sortSpilledBytes" : "X",
	"sortSpilledDataStorageSize" : "X",
	"sortSpilledRecords" : 50,
	"sortSpills" : 50,
	"usedDisk" : true
}
```

## 6. Group
### Pipeline
```json
[
	{
		"$group" : {
			"_id" : "$a",
			"b" : {
				"$sum" : "$b"
			}
		}
	},
	{
		"$sort" : {
			"b" : 1
		}
	}
]
```
### Slow query spilling stats
```json
{
	"groupSpilledBytes" : "X",
	"groupSpilledDataStorageSize" : "X",
	"groupSpilledRecords" : 4,
	"groupSpills" : 4,
	"sortSpilledBytes" : "X",
	"sortSpilledDataStorageSize" : "X",
	"sortSpilledRecords" : 4,
	"sortSpills" : 3,
	"usedDisk" : true
}
```

## 7. TextOr and projection
### Pipeline
```json
[
	{
		"$match" : {
			"$text" : {
				"$search" : "black tea"
			}
		}
	},
	{
		"$addFields" : {
			"score" : {
				"$meta" : "textScore"
			}
		}
	}
]
```
### Slow query spilling stats
```json
{
	"textOrSpilledBytes" : "X",
	"textOrSpilledDataStorageSize" : "X",
	"textOrSpilledRecords" : 4,
	"textOrSpills" : 4,
	"usedDisk" : true
}
```

## 8. TextOr and sort
### Pipeline
```json
[
	{
		"$match" : {
			"$text" : {
				"$search" : "black tea"
			}
		}
	},
	{
		"$sort" : {
			"_" : {
				"$meta" : "textScore"
			}
		}
	}
]
```
### Slow query spilling stats
```json
{
	"sortSpilledBytes" : "X",
	"sortSpilledDataStorageSize" : "X",
	"sortSpilledRecords" : 8,
	"sortSpills" : 5,
	"textOrSpilledBytes" : "X",
	"textOrSpilledDataStorageSize" : "X",
	"textOrSpilledRecords" : 4,
	"textOrSpills" : 4,
	"usedDisk" : true
}
```

## 9. BucketAuto
### Pipeline
```json
[
	{
		"$bucketAuto" : {
			"groupBy" : "$a",
			"buckets" : 2,
			"output" : {
				"sum" : {
					"$sum" : "$b"
				}
			}
		}
	}
]
```
### Slow query spilling stats
```json
{
	"bucketAutoSpilledBytes" : "X",
	"bucketAutoSpilledDataStorageSize" : "X",
	"bucketAutoSpilledRecords" : 13,
	"bucketAutoSpills" : 7,
	"usedDisk" : true
}
```

## 10. HashLookup
### Pipeline
```json
[
	{
		"$lookup" : {
			"from" : "logs_spilling_md_students",
			"localField" : "name",
			"foreignField" : "name",
			"as" : "matched"
		}
	}
]
```
### Slow query spilling stats
```json
{
	"hashLookupSpilledBytes" : "X",
	"hashLookupSpilledDataStorageSize" : "X",
	"hashLookupSpilledRecords" : 14,
	"hashLookupSpills" : 16,
	"usedDisk" : true
}
```

## 11. HashLookupUnwind
### Pipeline
```json
[
	{
		"$lookup" : {
			"from" : "logs_spilling_md_locations",
			"localField" : "locationName",
			"foreignField" : "name",
			"as" : "location"
		}
	},
	{
		"$unwind" : "$location"
	},
	{
		"$project" : {
			"locationName" : false,
			"location.extra" : false,
			"location.coordinates" : false,
			"colors" : false
		}
	}
]
```
### Slow query spilling stats
```json
{
	"hashLookupSpilledBytes" : "X",
	"hashLookupSpilledDataStorageSize" : "X",
	"hashLookupSpilledRecords" : 6,
	"hashLookupSpills" : 6,
	"usedDisk" : true
}
```

## 12. SetWindowFields
### Pipeline
```json
[
	{
		"$setWindowFields" : {
			"partitionBy" : "$a",
			"sortBy" : {
				"b" : 1
			},
			"output" : {
				"sum" : {
					"$sum" : "$b"
				}
			}
		}
	}
]
```
### Slow query spilling stats
```json
{
	"setWindowFieldsSpilledBytes" : "X",
	"setWindowFieldsSpilledDataStorageSize" : "X",
	"setWindowFieldsSpilledRecords" : 4,
	"setWindowFieldsSpills" : 4,
	"sortSpilledBytes" : "X",
	"sortSpilledDataStorageSize" : "X",
	"sortSpilledRecords" : 13,
	"sortSpills" : 7,
	"usedDisk" : true
}
```

### Pipeline
```json
[
	{
		"$setWindowFields" : {
			"partitionBy" : "$a",
			"sortBy" : {
				"b" : 1
			},
			"output" : {
				"sum" : {
					"$sum" : "$b"
				}
			}
		}
	},
	{
		"$limit" : 1
	}
]
```
### Slow query spilling stats
```json
{
	"setWindowFieldsSpilledBytes" : "X",
	"setWindowFieldsSpilledDataStorageSize" : "X",
	"setWindowFieldsSpilledRecords" : 3,
	"setWindowFieldsSpills" : 3,
	"sortSpilledBytes" : "X",
	"sortSpilledDataStorageSize" : "X",
	"sortSpilledRecords" : 13,
	"sortSpills" : 7,
	"usedDisk" : true
}
```

