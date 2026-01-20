## 1. Collection setup
Creating index and shard key
```json
{ "a" : 1 }
```
Split point
```json
{ "a" : 0 }
```
Inserting documents
```json
[
	{
		"_id" : 0,
		"m" : NumberInt(0),
		"a" : NumberInt(0)
	},
	{
		"_id" : 1,
		"m" : NumberInt(0),
		"a" : NumberInt(10)
	},
	{
		"_id" : 2,
		"m" : NumberInt(0),
		"a" : NumberInt(-10)
	}
]
```
### Pipeline
```json
[
	{
		"$sort" : {
			"m" : 1
		}
	},
	{
		"$addFields" : {
			"a" : NumberInt(0)
		}
	},
	{
		"$project" : {
			"_id" : 0,
			"a" : 1
		}
	}
]
```
### Results
```json
{  "a" : 0 }
{  "a" : 0 }
{  "a" : 0 }
```
