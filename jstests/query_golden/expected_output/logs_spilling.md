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

