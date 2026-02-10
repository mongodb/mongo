## 1. countries: [ ] - cities: [ ] - Indexes: [ ]

### Pipeline
```json
[
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : "$cities"
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

### Pipeline
```json
[
	{
		"$match" : {
			"_id" : {
				"$gt" : 0
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : "$cities"
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

### Pipeline
```json
[
	{
		"$match" : {
			"nonExistentField" : {
				"$exists" : false
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : "$cities"
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

### Pipeline
```json
[
	{
		"$match" : {
			"name" : "USA",
			"_id" : {
				"$gt" : 0
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : "$cities"
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

### Pipeline
```json
[
	{
		"$match" : {
			"name" : "Canada",
			"nonExistentField" : {
				"$exists" : false
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : "$cities"
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

### Pipeline
```json
[
	{
		"$match" : {
			"$and" : [
				{
					"name" : "France"
				},
				{
					"_id" : 3
				}
			]
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : "$cities"
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

### Pipeline
```json
[
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : {
			"path" : "$cities",
			"preserveNullAndEmptyArrays" : true
		}
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

### Pipeline
```json
[
	{
		"$match" : {
			"_id" : {
				"$gt" : 0
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : {
			"path" : "$cities",
			"preserveNullAndEmptyArrays" : true
		}
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

### Pipeline
```json
[
	{
		"$match" : {
			"nonExistentField" : {
				"$exists" : false
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : {
			"path" : "$cities",
			"preserveNullAndEmptyArrays" : true
		}
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

### Pipeline
```json
[
	{
		"$match" : {
			"name" : "USA",
			"_id" : {
				"$gt" : 0
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : {
			"path" : "$cities",
			"preserveNullAndEmptyArrays" : true
		}
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

### Pipeline
```json
[
	{
		"$match" : {
			"name" : "Canada",
			"nonExistentField" : {
				"$exists" : false
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : {
			"path" : "$cities",
			"preserveNullAndEmptyArrays" : true
		}
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

### Pipeline
```json
[
	{
		"$match" : {
			"$and" : [
				{
					"name" : "France"
				},
				{
					"_id" : 3
				}
			]
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : {
			"path" : "$cities",
			"preserveNullAndEmptyArrays" : true
		}
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

## 2. countries: [ ] - cities: [ ] - Indexes: [ { "collection" : "cities", "key" : { "countryId" : 1 } } ]

### Pipeline
```json
[
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : "$cities"
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

### Pipeline
```json
[
	{
		"$match" : {
			"_id" : {
				"$gt" : 0
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : "$cities"
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

### Pipeline
```json
[
	{
		"$match" : {
			"nonExistentField" : {
				"$exists" : false
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : "$cities"
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

### Pipeline
```json
[
	{
		"$match" : {
			"name" : "USA",
			"_id" : {
				"$gt" : 0
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : "$cities"
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

### Pipeline
```json
[
	{
		"$match" : {
			"name" : "Canada",
			"nonExistentField" : {
				"$exists" : false
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : "$cities"
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

### Pipeline
```json
[
	{
		"$match" : {
			"$and" : [
				{
					"name" : "France"
				},
				{
					"_id" : 3
				}
			]
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : "$cities"
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

### Pipeline
```json
[
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : {
			"path" : "$cities",
			"preserveNullAndEmptyArrays" : true
		}
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

### Pipeline
```json
[
	{
		"$match" : {
			"_id" : {
				"$gt" : 0
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : {
			"path" : "$cities",
			"preserveNullAndEmptyArrays" : true
		}
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

### Pipeline
```json
[
	{
		"$match" : {
			"nonExistentField" : {
				"$exists" : false
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : {
			"path" : "$cities",
			"preserveNullAndEmptyArrays" : true
		}
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

### Pipeline
```json
[
	{
		"$match" : {
			"name" : "USA",
			"_id" : {
				"$gt" : 0
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : {
			"path" : "$cities",
			"preserveNullAndEmptyArrays" : true
		}
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

### Pipeline
```json
[
	{
		"$match" : {
			"name" : "Canada",
			"nonExistentField" : {
				"$exists" : false
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : {
			"path" : "$cities",
			"preserveNullAndEmptyArrays" : true
		}
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

### Pipeline
```json
[
	{
		"$match" : {
			"$and" : [
				{
					"name" : "France"
				},
				{
					"_id" : 3
				}
			]
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : {
			"path" : "$cities",
			"preserveNullAndEmptyArrays" : true
		}
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

## 3. countries: [ ] - cities: [ ] - Indexes: [ { "collection" : "countries", "key" : { "name" : 1 } } ]

### Pipeline
```json
[
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : "$cities"
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

### Pipeline
```json
[
	{
		"$match" : {
			"_id" : {
				"$gt" : 0
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : "$cities"
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

### Pipeline
```json
[
	{
		"$match" : {
			"nonExistentField" : {
				"$exists" : false
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : "$cities"
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

### Pipeline
```json
[
	{
		"$match" : {
			"name" : "USA",
			"_id" : {
				"$gt" : 0
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : "$cities"
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

### Pipeline
```json
[
	{
		"$match" : {
			"name" : "Canada",
			"nonExistentField" : {
				"$exists" : false
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : "$cities"
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

### Pipeline
```json
[
	{
		"$match" : {
			"$and" : [
				{
					"name" : "France"
				},
				{
					"_id" : 3
				}
			]
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : "$cities"
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

### Pipeline
```json
[
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : {
			"path" : "$cities",
			"preserveNullAndEmptyArrays" : true
		}
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

### Pipeline
```json
[
	{
		"$match" : {
			"_id" : {
				"$gt" : 0
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : {
			"path" : "$cities",
			"preserveNullAndEmptyArrays" : true
		}
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

### Pipeline
```json
[
	{
		"$match" : {
			"nonExistentField" : {
				"$exists" : false
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : {
			"path" : "$cities",
			"preserveNullAndEmptyArrays" : true
		}
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

### Pipeline
```json
[
	{
		"$match" : {
			"name" : "USA",
			"_id" : {
				"$gt" : 0
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : {
			"path" : "$cities",
			"preserveNullAndEmptyArrays" : true
		}
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

### Pipeline
```json
[
	{
		"$match" : {
			"name" : "Canada",
			"nonExistentField" : {
				"$exists" : false
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : {
			"path" : "$cities",
			"preserveNullAndEmptyArrays" : true
		}
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

### Pipeline
```json
[
	{
		"$match" : {
			"$and" : [
				{
					"name" : "France"
				},
				{
					"_id" : 3
				}
			]
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : {
			"path" : "$cities",
			"preserveNullAndEmptyArrays" : true
		}
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

## 4. countries: [ ] - cities: [ ] - Indexes: [ { "collection" : "countries", "key" : { "_id" : 1 } } ]

### Pipeline
```json
[
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : "$cities"
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

### Pipeline
```json
[
	{
		"$match" : {
			"_id" : {
				"$gt" : 0
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : "$cities"
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

### Pipeline
```json
[
	{
		"$match" : {
			"nonExistentField" : {
				"$exists" : false
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : "$cities"
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

### Pipeline
```json
[
	{
		"$match" : {
			"name" : "USA",
			"_id" : {
				"$gt" : 0
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : "$cities"
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

### Pipeline
```json
[
	{
		"$match" : {
			"name" : "Canada",
			"nonExistentField" : {
				"$exists" : false
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : "$cities"
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

### Pipeline
```json
[
	{
		"$match" : {
			"$and" : [
				{
					"name" : "France"
				},
				{
					"_id" : 3
				}
			]
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : "$cities"
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

### Pipeline
```json
[
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : {
			"path" : "$cities",
			"preserveNullAndEmptyArrays" : true
		}
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

### Pipeline
```json
[
	{
		"$match" : {
			"_id" : {
				"$gt" : 0
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : {
			"path" : "$cities",
			"preserveNullAndEmptyArrays" : true
		}
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

### Pipeline
```json
[
	{
		"$match" : {
			"nonExistentField" : {
				"$exists" : false
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : {
			"path" : "$cities",
			"preserveNullAndEmptyArrays" : true
		}
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

### Pipeline
```json
[
	{
		"$match" : {
			"name" : "USA",
			"_id" : {
				"$gt" : 0
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : {
			"path" : "$cities",
			"preserveNullAndEmptyArrays" : true
		}
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

### Pipeline
```json
[
	{
		"$match" : {
			"name" : "Canada",
			"nonExistentField" : {
				"$exists" : false
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : {
			"path" : "$cities",
			"preserveNullAndEmptyArrays" : true
		}
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

### Pipeline
```json
[
	{
		"$match" : {
			"$and" : [
				{
					"name" : "France"
				},
				{
					"_id" : 3
				}
			]
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : {
			"path" : "$cities",
			"preserveNullAndEmptyArrays" : true
		}
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

## 5. countries: [ ] - cities: [ { "_id" : 10, "countryId" : 1, "cityName" : "New York" }, { "_id" : 11, "countryId" : 1, "cityName" : "Los Angeles" }, { "_id" : 12, "countryId" : 2, "cityName" : "Toronto" }, { "_id" : 13, "countryId" : 3, "cityName" : "Paris" } ] - Indexes: [ ]

### Pipeline
```json
[
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : "$cities"
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

### Pipeline
```json
[
	{
		"$match" : {
			"_id" : {
				"$gt" : 0
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : "$cities"
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

### Pipeline
```json
[
	{
		"$match" : {
			"nonExistentField" : {
				"$exists" : false
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : "$cities"
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

### Pipeline
```json
[
	{
		"$match" : {
			"name" : "USA",
			"_id" : {
				"$gt" : 0
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : "$cities"
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

### Pipeline
```json
[
	{
		"$match" : {
			"name" : "Canada",
			"nonExistentField" : {
				"$exists" : false
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : "$cities"
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

### Pipeline
```json
[
	{
		"$match" : {
			"$and" : [
				{
					"name" : "France"
				},
				{
					"_id" : 3
				}
			]
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : "$cities"
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

### Pipeline
```json
[
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : {
			"path" : "$cities",
			"preserveNullAndEmptyArrays" : true
		}
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

### Pipeline
```json
[
	{
		"$match" : {
			"_id" : {
				"$gt" : 0
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : {
			"path" : "$cities",
			"preserveNullAndEmptyArrays" : true
		}
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

### Pipeline
```json
[
	{
		"$match" : {
			"nonExistentField" : {
				"$exists" : false
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : {
			"path" : "$cities",
			"preserveNullAndEmptyArrays" : true
		}
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

### Pipeline
```json
[
	{
		"$match" : {
			"name" : "USA",
			"_id" : {
				"$gt" : 0
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : {
			"path" : "$cities",
			"preserveNullAndEmptyArrays" : true
		}
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

### Pipeline
```json
[
	{
		"$match" : {
			"name" : "Canada",
			"nonExistentField" : {
				"$exists" : false
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : {
			"path" : "$cities",
			"preserveNullAndEmptyArrays" : true
		}
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

### Pipeline
```json
[
	{
		"$match" : {
			"$and" : [
				{
					"name" : "France"
				},
				{
					"_id" : 3
				}
			]
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : {
			"path" : "$cities",
			"preserveNullAndEmptyArrays" : true
		}
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

## 6. countries: [ ] - cities: [ { "_id" : 10, "countryId" : 1, "cityName" : "New York" }, { "_id" : 11, "countryId" : 1, "cityName" : "Los Angeles" }, { "_id" : 12, "countryId" : 2, "cityName" : "Toronto" }, { "_id" : 13, "countryId" : 3, "cityName" : "Paris" } ] - Indexes: [ { "collection" : "cities", "key" : { "countryId" : 1 } } ]

### Pipeline
```json
[
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : "$cities"
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

### Pipeline
```json
[
	{
		"$match" : {
			"_id" : {
				"$gt" : 0
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : "$cities"
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

### Pipeline
```json
[
	{
		"$match" : {
			"nonExistentField" : {
				"$exists" : false
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : "$cities"
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

### Pipeline
```json
[
	{
		"$match" : {
			"name" : "USA",
			"_id" : {
				"$gt" : 0
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : "$cities"
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

### Pipeline
```json
[
	{
		"$match" : {
			"name" : "Canada",
			"nonExistentField" : {
				"$exists" : false
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : "$cities"
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

### Pipeline
```json
[
	{
		"$match" : {
			"$and" : [
				{
					"name" : "France"
				},
				{
					"_id" : 3
				}
			]
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : "$cities"
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

### Pipeline
```json
[
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : {
			"path" : "$cities",
			"preserveNullAndEmptyArrays" : true
		}
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

### Pipeline
```json
[
	{
		"$match" : {
			"_id" : {
				"$gt" : 0
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : {
			"path" : "$cities",
			"preserveNullAndEmptyArrays" : true
		}
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

### Pipeline
```json
[
	{
		"$match" : {
			"nonExistentField" : {
				"$exists" : false
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : {
			"path" : "$cities",
			"preserveNullAndEmptyArrays" : true
		}
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

### Pipeline
```json
[
	{
		"$match" : {
			"name" : "USA",
			"_id" : {
				"$gt" : 0
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : {
			"path" : "$cities",
			"preserveNullAndEmptyArrays" : true
		}
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

### Pipeline
```json
[
	{
		"$match" : {
			"name" : "Canada",
			"nonExistentField" : {
				"$exists" : false
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : {
			"path" : "$cities",
			"preserveNullAndEmptyArrays" : true
		}
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

### Pipeline
```json
[
	{
		"$match" : {
			"$and" : [
				{
					"name" : "France"
				},
				{
					"_id" : 3
				}
			]
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : {
			"path" : "$cities",
			"preserveNullAndEmptyArrays" : true
		}
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

## 7. countries: [ ] - cities: [ { "_id" : 10, "countryId" : 1, "cityName" : "New York" }, { "_id" : 11, "countryId" : 1, "cityName" : "Los Angeles" }, { "_id" : 12, "countryId" : 2, "cityName" : "Toronto" }, { "_id" : 13, "countryId" : 3, "cityName" : "Paris" } ] - Indexes: [ { "collection" : "countries", "key" : { "name" : 1 } } ]

### Pipeline
```json
[
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : "$cities"
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

### Pipeline
```json
[
	{
		"$match" : {
			"_id" : {
				"$gt" : 0
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : "$cities"
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

### Pipeline
```json
[
	{
		"$match" : {
			"nonExistentField" : {
				"$exists" : false
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : "$cities"
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

### Pipeline
```json
[
	{
		"$match" : {
			"name" : "USA",
			"_id" : {
				"$gt" : 0
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : "$cities"
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

### Pipeline
```json
[
	{
		"$match" : {
			"name" : "Canada",
			"nonExistentField" : {
				"$exists" : false
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : "$cities"
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

### Pipeline
```json
[
	{
		"$match" : {
			"$and" : [
				{
					"name" : "France"
				},
				{
					"_id" : 3
				}
			]
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : "$cities"
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

### Pipeline
```json
[
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : {
			"path" : "$cities",
			"preserveNullAndEmptyArrays" : true
		}
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

### Pipeline
```json
[
	{
		"$match" : {
			"_id" : {
				"$gt" : 0
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : {
			"path" : "$cities",
			"preserveNullAndEmptyArrays" : true
		}
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

### Pipeline
```json
[
	{
		"$match" : {
			"nonExistentField" : {
				"$exists" : false
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : {
			"path" : "$cities",
			"preserveNullAndEmptyArrays" : true
		}
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

### Pipeline
```json
[
	{
		"$match" : {
			"name" : "USA",
			"_id" : {
				"$gt" : 0
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : {
			"path" : "$cities",
			"preserveNullAndEmptyArrays" : true
		}
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

### Pipeline
```json
[
	{
		"$match" : {
			"name" : "Canada",
			"nonExistentField" : {
				"$exists" : false
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : {
			"path" : "$cities",
			"preserveNullAndEmptyArrays" : true
		}
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

### Pipeline
```json
[
	{
		"$match" : {
			"$and" : [
				{
					"name" : "France"
				},
				{
					"_id" : 3
				}
			]
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : {
			"path" : "$cities",
			"preserveNullAndEmptyArrays" : true
		}
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

## 8. countries: [ ] - cities: [ { "_id" : 10, "countryId" : 1, "cityName" : "New York" }, { "_id" : 11, "countryId" : 1, "cityName" : "Los Angeles" }, { "_id" : 12, "countryId" : 2, "cityName" : "Toronto" }, { "_id" : 13, "countryId" : 3, "cityName" : "Paris" } ] - Indexes: [ { "collection" : "countries", "key" : { "_id" : 1 } } ]

### Pipeline
```json
[
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : "$cities"
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

### Pipeline
```json
[
	{
		"$match" : {
			"_id" : {
				"$gt" : 0
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : "$cities"
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

### Pipeline
```json
[
	{
		"$match" : {
			"nonExistentField" : {
				"$exists" : false
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : "$cities"
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

### Pipeline
```json
[
	{
		"$match" : {
			"name" : "USA",
			"_id" : {
				"$gt" : 0
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : "$cities"
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

### Pipeline
```json
[
	{
		"$match" : {
			"name" : "Canada",
			"nonExistentField" : {
				"$exists" : false
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : "$cities"
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

### Pipeline
```json
[
	{
		"$match" : {
			"$and" : [
				{
					"name" : "France"
				},
				{
					"_id" : 3
				}
			]
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : "$cities"
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

### Pipeline
```json
[
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : {
			"path" : "$cities",
			"preserveNullAndEmptyArrays" : true
		}
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

### Pipeline
```json
[
	{
		"$match" : {
			"_id" : {
				"$gt" : 0
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : {
			"path" : "$cities",
			"preserveNullAndEmptyArrays" : true
		}
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

### Pipeline
```json
[
	{
		"$match" : {
			"nonExistentField" : {
				"$exists" : false
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : {
			"path" : "$cities",
			"preserveNullAndEmptyArrays" : true
		}
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

### Pipeline
```json
[
	{
		"$match" : {
			"name" : "USA",
			"_id" : {
				"$gt" : 0
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : {
			"path" : "$cities",
			"preserveNullAndEmptyArrays" : true
		}
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

### Pipeline
```json
[
	{
		"$match" : {
			"name" : "Canada",
			"nonExistentField" : {
				"$exists" : false
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : {
			"path" : "$cities",
			"preserveNullAndEmptyArrays" : true
		}
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

### Pipeline
```json
[
	{
		"$match" : {
			"$and" : [
				{
					"name" : "France"
				},
				{
					"_id" : 3
				}
			]
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : {
			"path" : "$cities",
			"preserveNullAndEmptyArrays" : true
		}
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

## 9. countries: [ { "_id" : 1, "name" : "USA" }, { "_id" : 2, "name" : "Canada" }, { "_id" : 3, "name" : "France" }, { "_id" : 4, "name" : "Romania" } ] - cities: [ ] - Indexes: [ ]

### Pipeline
```json
[
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : "$cities"
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

### Pipeline
```json
[
	{
		"$match" : {
			"_id" : {
				"$gt" : 0
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : "$cities"
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

### Pipeline
```json
[
	{
		"$match" : {
			"nonExistentField" : {
				"$exists" : false
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : "$cities"
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

### Pipeline
```json
[
	{
		"$match" : {
			"name" : "USA",
			"_id" : {
				"$gt" : 0
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : "$cities"
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

### Pipeline
```json
[
	{
		"$match" : {
			"name" : "Canada",
			"nonExistentField" : {
				"$exists" : false
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : "$cities"
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

### Pipeline
```json
[
	{
		"$match" : {
			"$and" : [
				{
					"name" : "France"
				},
				{
					"_id" : 3
				}
			]
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : "$cities"
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

### Pipeline
```json
[
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : {
			"path" : "$cities",
			"preserveNullAndEmptyArrays" : true
		}
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json
{ "_id" : 1, "name" : "USA" }
{ "_id" : 2, "name" : "Canada" }
{ "_id" : 3, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : true } - Iteration 1
```json
{ "_id" : 1, "name" : "USA" }
{ "_id" : 2, "name" : "Canada" }
{ "_id" : 3, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : true } - Iteration 2
```json
{ "_id" : 1, "name" : "USA" }
{ "_id" : 2, "name" : "Canada" }
{ "_id" : 3, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : false } - Iteration 0
```json
{ "_id" : 1, "name" : "USA" }
{ "_id" : 2, "name" : "Canada" }
{ "_id" : 3, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : false } - Iteration 1
```json
{ "_id" : 1, "name" : "USA" }
{ "_id" : 2, "name" : "Canada" }
{ "_id" : 3, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : false } - Iteration 2
```json
{ "_id" : 1, "name" : "USA" }
{ "_id" : 2, "name" : "Canada" }
{ "_id" : 3, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```

### Pipeline
```json
[
	{
		"$match" : {
			"_id" : {
				"$gt" : 0
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : {
			"path" : "$cities",
			"preserveNullAndEmptyArrays" : true
		}
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json
{ "_id" : 1, "name" : "USA" }
{ "_id" : 2, "name" : "Canada" }
{ "_id" : 3, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : true } - Iteration 1
```json
{ "_id" : 1, "name" : "USA" }
{ "_id" : 2, "name" : "Canada" }
{ "_id" : 3, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : true } - Iteration 2
```json
{ "_id" : 1, "name" : "USA" }
{ "_id" : 2, "name" : "Canada" }
{ "_id" : 3, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : false } - Iteration 0
```json
{ "_id" : 1, "name" : "USA" }
{ "_id" : 2, "name" : "Canada" }
{ "_id" : 3, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : false } - Iteration 1
```json
{ "_id" : 1, "name" : "USA" }
{ "_id" : 2, "name" : "Canada" }
{ "_id" : 3, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : false } - Iteration 2
```json
{ "_id" : 1, "name" : "USA" }
{ "_id" : 2, "name" : "Canada" }
{ "_id" : 3, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```

### Pipeline
```json
[
	{
		"$match" : {
			"nonExistentField" : {
				"$exists" : false
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : {
			"path" : "$cities",
			"preserveNullAndEmptyArrays" : true
		}
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json
{ "_id" : 1, "name" : "USA" }
{ "_id" : 2, "name" : "Canada" }
{ "_id" : 3, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : true } - Iteration 1
```json
{ "_id" : 1, "name" : "USA" }
{ "_id" : 2, "name" : "Canada" }
{ "_id" : 3, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : true } - Iteration 2
```json
{ "_id" : 1, "name" : "USA" }
{ "_id" : 2, "name" : "Canada" }
{ "_id" : 3, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : false } - Iteration 0
```json
{ "_id" : 1, "name" : "USA" }
{ "_id" : 2, "name" : "Canada" }
{ "_id" : 3, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : false } - Iteration 1
```json
{ "_id" : 1, "name" : "USA" }
{ "_id" : 2, "name" : "Canada" }
{ "_id" : 3, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : false } - Iteration 2
```json
{ "_id" : 1, "name" : "USA" }
{ "_id" : 2, "name" : "Canada" }
{ "_id" : 3, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```

### Pipeline
```json
[
	{
		"$match" : {
			"name" : "USA",
			"_id" : {
				"$gt" : 0
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : {
			"path" : "$cities",
			"preserveNullAndEmptyArrays" : true
		}
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json
{ "_id" : 1, "name" : "USA" }
```
### Options: { "allowDiskUse" : true } - Iteration 1
```json
{ "_id" : 1, "name" : "USA" }
```
### Options: { "allowDiskUse" : true } - Iteration 2
```json
{ "_id" : 1, "name" : "USA" }
```
### Options: { "allowDiskUse" : false } - Iteration 0
```json
{ "_id" : 1, "name" : "USA" }
```
### Options: { "allowDiskUse" : false } - Iteration 1
```json
{ "_id" : 1, "name" : "USA" }
```
### Options: { "allowDiskUse" : false } - Iteration 2
```json
{ "_id" : 1, "name" : "USA" }
```

### Pipeline
```json
[
	{
		"$match" : {
			"name" : "Canada",
			"nonExistentField" : {
				"$exists" : false
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : {
			"path" : "$cities",
			"preserveNullAndEmptyArrays" : true
		}
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json
{ "_id" : 2, "name" : "Canada" }
```
### Options: { "allowDiskUse" : true } - Iteration 1
```json
{ "_id" : 2, "name" : "Canada" }
```
### Options: { "allowDiskUse" : true } - Iteration 2
```json
{ "_id" : 2, "name" : "Canada" }
```
### Options: { "allowDiskUse" : false } - Iteration 0
```json
{ "_id" : 2, "name" : "Canada" }
```
### Options: { "allowDiskUse" : false } - Iteration 1
```json
{ "_id" : 2, "name" : "Canada" }
```
### Options: { "allowDiskUse" : false } - Iteration 2
```json
{ "_id" : 2, "name" : "Canada" }
```

### Pipeline
```json
[
	{
		"$match" : {
			"$and" : [
				{
					"name" : "France"
				},
				{
					"_id" : 3
				}
			]
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : {
			"path" : "$cities",
			"preserveNullAndEmptyArrays" : true
		}
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json
{ "_id" : 3, "name" : "France" }
```
### Options: { "allowDiskUse" : true } - Iteration 1
```json
{ "_id" : 3, "name" : "France" }
```
### Options: { "allowDiskUse" : true } - Iteration 2
```json
{ "_id" : 3, "name" : "France" }
```
### Options: { "allowDiskUse" : false } - Iteration 0
```json
{ "_id" : 3, "name" : "France" }
```
### Options: { "allowDiskUse" : false } - Iteration 1
```json
{ "_id" : 3, "name" : "France" }
```
### Options: { "allowDiskUse" : false } - Iteration 2
```json
{ "_id" : 3, "name" : "France" }
```

## 10. countries: [ { "_id" : 1, "name" : "USA" }, { "_id" : 2, "name" : "Canada" }, { "_id" : 3, "name" : "France" }, { "_id" : 4, "name" : "Romania" } ] - cities: [ ] - Indexes: [ { "collection" : "cities", "key" : { "countryId" : 1 } } ]

### Pipeline
```json
[
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : "$cities"
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

### Pipeline
```json
[
	{
		"$match" : {
			"_id" : {
				"$gt" : 0
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : "$cities"
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

### Pipeline
```json
[
	{
		"$match" : {
			"nonExistentField" : {
				"$exists" : false
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : "$cities"
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

### Pipeline
```json
[
	{
		"$match" : {
			"name" : "USA",
			"_id" : {
				"$gt" : 0
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : "$cities"
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

### Pipeline
```json
[
	{
		"$match" : {
			"name" : "Canada",
			"nonExistentField" : {
				"$exists" : false
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : "$cities"
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

### Pipeline
```json
[
	{
		"$match" : {
			"$and" : [
				{
					"name" : "France"
				},
				{
					"_id" : 3
				}
			]
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : "$cities"
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

### Pipeline
```json
[
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : {
			"path" : "$cities",
			"preserveNullAndEmptyArrays" : true
		}
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json
{ "_id" : 1, "name" : "USA" }
{ "_id" : 2, "name" : "Canada" }
{ "_id" : 3, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : true } - Iteration 1
```json
{ "_id" : 1, "name" : "USA" }
{ "_id" : 2, "name" : "Canada" }
{ "_id" : 3, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : true } - Iteration 2
```json
{ "_id" : 1, "name" : "USA" }
{ "_id" : 2, "name" : "Canada" }
{ "_id" : 3, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : false } - Iteration 0
```json
{ "_id" : 1, "name" : "USA" }
{ "_id" : 2, "name" : "Canada" }
{ "_id" : 3, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : false } - Iteration 1
```json
{ "_id" : 1, "name" : "USA" }
{ "_id" : 2, "name" : "Canada" }
{ "_id" : 3, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : false } - Iteration 2
```json
{ "_id" : 1, "name" : "USA" }
{ "_id" : 2, "name" : "Canada" }
{ "_id" : 3, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```

### Pipeline
```json
[
	{
		"$match" : {
			"_id" : {
				"$gt" : 0
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : {
			"path" : "$cities",
			"preserveNullAndEmptyArrays" : true
		}
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json
{ "_id" : 1, "name" : "USA" }
{ "_id" : 2, "name" : "Canada" }
{ "_id" : 3, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : true } - Iteration 1
```json
{ "_id" : 1, "name" : "USA" }
{ "_id" : 2, "name" : "Canada" }
{ "_id" : 3, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : true } - Iteration 2
```json
{ "_id" : 1, "name" : "USA" }
{ "_id" : 2, "name" : "Canada" }
{ "_id" : 3, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : false } - Iteration 0
```json
{ "_id" : 1, "name" : "USA" }
{ "_id" : 2, "name" : "Canada" }
{ "_id" : 3, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : false } - Iteration 1
```json
{ "_id" : 1, "name" : "USA" }
{ "_id" : 2, "name" : "Canada" }
{ "_id" : 3, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : false } - Iteration 2
```json
{ "_id" : 1, "name" : "USA" }
{ "_id" : 2, "name" : "Canada" }
{ "_id" : 3, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```

### Pipeline
```json
[
	{
		"$match" : {
			"nonExistentField" : {
				"$exists" : false
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : {
			"path" : "$cities",
			"preserveNullAndEmptyArrays" : true
		}
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json
{ "_id" : 1, "name" : "USA" }
{ "_id" : 2, "name" : "Canada" }
{ "_id" : 3, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : true } - Iteration 1
```json
{ "_id" : 1, "name" : "USA" }
{ "_id" : 2, "name" : "Canada" }
{ "_id" : 3, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : true } - Iteration 2
```json
{ "_id" : 1, "name" : "USA" }
{ "_id" : 2, "name" : "Canada" }
{ "_id" : 3, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : false } - Iteration 0
```json
{ "_id" : 1, "name" : "USA" }
{ "_id" : 2, "name" : "Canada" }
{ "_id" : 3, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : false } - Iteration 1
```json
{ "_id" : 1, "name" : "USA" }
{ "_id" : 2, "name" : "Canada" }
{ "_id" : 3, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : false } - Iteration 2
```json
{ "_id" : 1, "name" : "USA" }
{ "_id" : 2, "name" : "Canada" }
{ "_id" : 3, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```

### Pipeline
```json
[
	{
		"$match" : {
			"name" : "USA",
			"_id" : {
				"$gt" : 0
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : {
			"path" : "$cities",
			"preserveNullAndEmptyArrays" : true
		}
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json
{ "_id" : 1, "name" : "USA" }
```
### Options: { "allowDiskUse" : true } - Iteration 1
```json
{ "_id" : 1, "name" : "USA" }
```
### Options: { "allowDiskUse" : true } - Iteration 2
```json
{ "_id" : 1, "name" : "USA" }
```
### Options: { "allowDiskUse" : false } - Iteration 0
```json
{ "_id" : 1, "name" : "USA" }
```
### Options: { "allowDiskUse" : false } - Iteration 1
```json
{ "_id" : 1, "name" : "USA" }
```
### Options: { "allowDiskUse" : false } - Iteration 2
```json
{ "_id" : 1, "name" : "USA" }
```

### Pipeline
```json
[
	{
		"$match" : {
			"name" : "Canada",
			"nonExistentField" : {
				"$exists" : false
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : {
			"path" : "$cities",
			"preserveNullAndEmptyArrays" : true
		}
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json
{ "_id" : 2, "name" : "Canada" }
```
### Options: { "allowDiskUse" : true } - Iteration 1
```json
{ "_id" : 2, "name" : "Canada" }
```
### Options: { "allowDiskUse" : true } - Iteration 2
```json
{ "_id" : 2, "name" : "Canada" }
```
### Options: { "allowDiskUse" : false } - Iteration 0
```json
{ "_id" : 2, "name" : "Canada" }
```
### Options: { "allowDiskUse" : false } - Iteration 1
```json
{ "_id" : 2, "name" : "Canada" }
```
### Options: { "allowDiskUse" : false } - Iteration 2
```json
{ "_id" : 2, "name" : "Canada" }
```

### Pipeline
```json
[
	{
		"$match" : {
			"$and" : [
				{
					"name" : "France"
				},
				{
					"_id" : 3
				}
			]
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : {
			"path" : "$cities",
			"preserveNullAndEmptyArrays" : true
		}
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json
{ "_id" : 3, "name" : "France" }
```
### Options: { "allowDiskUse" : true } - Iteration 1
```json
{ "_id" : 3, "name" : "France" }
```
### Options: { "allowDiskUse" : true } - Iteration 2
```json
{ "_id" : 3, "name" : "France" }
```
### Options: { "allowDiskUse" : false } - Iteration 0
```json
{ "_id" : 3, "name" : "France" }
```
### Options: { "allowDiskUse" : false } - Iteration 1
```json
{ "_id" : 3, "name" : "France" }
```
### Options: { "allowDiskUse" : false } - Iteration 2
```json
{ "_id" : 3, "name" : "France" }
```

## 11. countries: [ { "_id" : 1, "name" : "USA" }, { "_id" : 2, "name" : "Canada" }, { "_id" : 3, "name" : "France" }, { "_id" : 4, "name" : "Romania" } ] - cities: [ ] - Indexes: [ { "collection" : "countries", "key" : { "name" : 1 } } ]

### Pipeline
```json
[
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : "$cities"
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

### Pipeline
```json
[
	{
		"$match" : {
			"_id" : {
				"$gt" : 0
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : "$cities"
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

### Pipeline
```json
[
	{
		"$match" : {
			"nonExistentField" : {
				"$exists" : false
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : "$cities"
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

### Pipeline
```json
[
	{
		"$match" : {
			"name" : "USA",
			"_id" : {
				"$gt" : 0
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : "$cities"
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

### Pipeline
```json
[
	{
		"$match" : {
			"name" : "Canada",
			"nonExistentField" : {
				"$exists" : false
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : "$cities"
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

### Pipeline
```json
[
	{
		"$match" : {
			"$and" : [
				{
					"name" : "France"
				},
				{
					"_id" : 3
				}
			]
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : "$cities"
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

### Pipeline
```json
[
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : {
			"path" : "$cities",
			"preserveNullAndEmptyArrays" : true
		}
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json
{ "_id" : 1, "name" : "USA" }
{ "_id" : 2, "name" : "Canada" }
{ "_id" : 3, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : true } - Iteration 1
```json
{ "_id" : 1, "name" : "USA" }
{ "_id" : 2, "name" : "Canada" }
{ "_id" : 3, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : true } - Iteration 2
```json
{ "_id" : 1, "name" : "USA" }
{ "_id" : 2, "name" : "Canada" }
{ "_id" : 3, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : false } - Iteration 0
```json
{ "_id" : 1, "name" : "USA" }
{ "_id" : 2, "name" : "Canada" }
{ "_id" : 3, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : false } - Iteration 1
```json
{ "_id" : 1, "name" : "USA" }
{ "_id" : 2, "name" : "Canada" }
{ "_id" : 3, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : false } - Iteration 2
```json
{ "_id" : 1, "name" : "USA" }
{ "_id" : 2, "name" : "Canada" }
{ "_id" : 3, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```

### Pipeline
```json
[
	{
		"$match" : {
			"_id" : {
				"$gt" : 0
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : {
			"path" : "$cities",
			"preserveNullAndEmptyArrays" : true
		}
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json
{ "_id" : 1, "name" : "USA" }
{ "_id" : 2, "name" : "Canada" }
{ "_id" : 3, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : true } - Iteration 1
```json
{ "_id" : 1, "name" : "USA" }
{ "_id" : 2, "name" : "Canada" }
{ "_id" : 3, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : true } - Iteration 2
```json
{ "_id" : 1, "name" : "USA" }
{ "_id" : 2, "name" : "Canada" }
{ "_id" : 3, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : false } - Iteration 0
```json
{ "_id" : 1, "name" : "USA" }
{ "_id" : 2, "name" : "Canada" }
{ "_id" : 3, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : false } - Iteration 1
```json
{ "_id" : 1, "name" : "USA" }
{ "_id" : 2, "name" : "Canada" }
{ "_id" : 3, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : false } - Iteration 2
```json
{ "_id" : 1, "name" : "USA" }
{ "_id" : 2, "name" : "Canada" }
{ "_id" : 3, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```

### Pipeline
```json
[
	{
		"$match" : {
			"nonExistentField" : {
				"$exists" : false
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : {
			"path" : "$cities",
			"preserveNullAndEmptyArrays" : true
		}
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json
{ "_id" : 1, "name" : "USA" }
{ "_id" : 2, "name" : "Canada" }
{ "_id" : 3, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : true } - Iteration 1
```json
{ "_id" : 1, "name" : "USA" }
{ "_id" : 2, "name" : "Canada" }
{ "_id" : 3, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : true } - Iteration 2
```json
{ "_id" : 1, "name" : "USA" }
{ "_id" : 2, "name" : "Canada" }
{ "_id" : 3, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : false } - Iteration 0
```json
{ "_id" : 1, "name" : "USA" }
{ "_id" : 2, "name" : "Canada" }
{ "_id" : 3, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : false } - Iteration 1
```json
{ "_id" : 1, "name" : "USA" }
{ "_id" : 2, "name" : "Canada" }
{ "_id" : 3, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : false } - Iteration 2
```json
{ "_id" : 1, "name" : "USA" }
{ "_id" : 2, "name" : "Canada" }
{ "_id" : 3, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```

### Pipeline
```json
[
	{
		"$match" : {
			"name" : "USA",
			"_id" : {
				"$gt" : 0
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : {
			"path" : "$cities",
			"preserveNullAndEmptyArrays" : true
		}
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json
{ "_id" : 1, "name" : "USA" }
```
### Options: { "allowDiskUse" : true } - Iteration 1
```json
{ "_id" : 1, "name" : "USA" }
```
### Options: { "allowDiskUse" : true } - Iteration 2
```json
{ "_id" : 1, "name" : "USA" }
```
### Options: { "allowDiskUse" : false } - Iteration 0
```json
{ "_id" : 1, "name" : "USA" }
```
### Options: { "allowDiskUse" : false } - Iteration 1
```json
{ "_id" : 1, "name" : "USA" }
```
### Options: { "allowDiskUse" : false } - Iteration 2
```json
{ "_id" : 1, "name" : "USA" }
```

### Pipeline
```json
[
	{
		"$match" : {
			"name" : "Canada",
			"nonExistentField" : {
				"$exists" : false
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : {
			"path" : "$cities",
			"preserveNullAndEmptyArrays" : true
		}
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json
{ "_id" : 2, "name" : "Canada" }
```
### Options: { "allowDiskUse" : true } - Iteration 1
```json
{ "_id" : 2, "name" : "Canada" }
```
### Options: { "allowDiskUse" : true } - Iteration 2
```json
{ "_id" : 2, "name" : "Canada" }
```
### Options: { "allowDiskUse" : false } - Iteration 0
```json
{ "_id" : 2, "name" : "Canada" }
```
### Options: { "allowDiskUse" : false } - Iteration 1
```json
{ "_id" : 2, "name" : "Canada" }
```
### Options: { "allowDiskUse" : false } - Iteration 2
```json
{ "_id" : 2, "name" : "Canada" }
```

### Pipeline
```json
[
	{
		"$match" : {
			"$and" : [
				{
					"name" : "France"
				},
				{
					"_id" : 3
				}
			]
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : {
			"path" : "$cities",
			"preserveNullAndEmptyArrays" : true
		}
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json
{ "_id" : 3, "name" : "France" }
```
### Options: { "allowDiskUse" : true } - Iteration 1
```json
{ "_id" : 3, "name" : "France" }
```
### Options: { "allowDiskUse" : true } - Iteration 2
```json
{ "_id" : 3, "name" : "France" }
```
### Options: { "allowDiskUse" : false } - Iteration 0
```json
{ "_id" : 3, "name" : "France" }
```
### Options: { "allowDiskUse" : false } - Iteration 1
```json
{ "_id" : 3, "name" : "France" }
```
### Options: { "allowDiskUse" : false } - Iteration 2
```json
{ "_id" : 3, "name" : "France" }
```

## 12. countries: [ { "_id" : 1, "name" : "USA" }, { "_id" : 2, "name" : "Canada" }, { "_id" : 3, "name" : "France" }, { "_id" : 4, "name" : "Romania" } ] - cities: [ ] - Indexes: [ { "collection" : "countries", "key" : { "_id" : 1 } } ]

### Pipeline
```json
[
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : "$cities"
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

### Pipeline
```json
[
	{
		"$match" : {
			"_id" : {
				"$gt" : 0
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : "$cities"
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

### Pipeline
```json
[
	{
		"$match" : {
			"nonExistentField" : {
				"$exists" : false
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : "$cities"
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

### Pipeline
```json
[
	{
		"$match" : {
			"name" : "USA",
			"_id" : {
				"$gt" : 0
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : "$cities"
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

### Pipeline
```json
[
	{
		"$match" : {
			"name" : "Canada",
			"nonExistentField" : {
				"$exists" : false
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : "$cities"
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

### Pipeline
```json
[
	{
		"$match" : {
			"$and" : [
				{
					"name" : "France"
				},
				{
					"_id" : 3
				}
			]
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : "$cities"
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json

```
### Options: { "allowDiskUse" : true } - Iteration 1
```json

```
### Options: { "allowDiskUse" : true } - Iteration 2
```json

```
### Options: { "allowDiskUse" : false } - Iteration 0
```json

```
### Options: { "allowDiskUse" : false } - Iteration 1
```json

```
### Options: { "allowDiskUse" : false } - Iteration 2
```json

```

### Pipeline
```json
[
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : {
			"path" : "$cities",
			"preserveNullAndEmptyArrays" : true
		}
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json
{ "_id" : 1, "name" : "USA" }
{ "_id" : 2, "name" : "Canada" }
{ "_id" : 3, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : true } - Iteration 1
```json
{ "_id" : 1, "name" : "USA" }
{ "_id" : 2, "name" : "Canada" }
{ "_id" : 3, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : true } - Iteration 2
```json
{ "_id" : 1, "name" : "USA" }
{ "_id" : 2, "name" : "Canada" }
{ "_id" : 3, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : false } - Iteration 0
```json
{ "_id" : 1, "name" : "USA" }
{ "_id" : 2, "name" : "Canada" }
{ "_id" : 3, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : false } - Iteration 1
```json
{ "_id" : 1, "name" : "USA" }
{ "_id" : 2, "name" : "Canada" }
{ "_id" : 3, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : false } - Iteration 2
```json
{ "_id" : 1, "name" : "USA" }
{ "_id" : 2, "name" : "Canada" }
{ "_id" : 3, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```

### Pipeline
```json
[
	{
		"$match" : {
			"_id" : {
				"$gt" : 0
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : {
			"path" : "$cities",
			"preserveNullAndEmptyArrays" : true
		}
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json
{ "_id" : 1, "name" : "USA" }
{ "_id" : 2, "name" : "Canada" }
{ "_id" : 3, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : true } - Iteration 1
```json
{ "_id" : 1, "name" : "USA" }
{ "_id" : 2, "name" : "Canada" }
{ "_id" : 3, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : true } - Iteration 2
```json
{ "_id" : 1, "name" : "USA" }
{ "_id" : 2, "name" : "Canada" }
{ "_id" : 3, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : false } - Iteration 0
```json
{ "_id" : 1, "name" : "USA" }
{ "_id" : 2, "name" : "Canada" }
{ "_id" : 3, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : false } - Iteration 1
```json
{ "_id" : 1, "name" : "USA" }
{ "_id" : 2, "name" : "Canada" }
{ "_id" : 3, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : false } - Iteration 2
```json
{ "_id" : 1, "name" : "USA" }
{ "_id" : 2, "name" : "Canada" }
{ "_id" : 3, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```

### Pipeline
```json
[
	{
		"$match" : {
			"nonExistentField" : {
				"$exists" : false
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : {
			"path" : "$cities",
			"preserveNullAndEmptyArrays" : true
		}
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json
{ "_id" : 1, "name" : "USA" }
{ "_id" : 2, "name" : "Canada" }
{ "_id" : 3, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : true } - Iteration 1
```json
{ "_id" : 1, "name" : "USA" }
{ "_id" : 2, "name" : "Canada" }
{ "_id" : 3, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : true } - Iteration 2
```json
{ "_id" : 1, "name" : "USA" }
{ "_id" : 2, "name" : "Canada" }
{ "_id" : 3, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : false } - Iteration 0
```json
{ "_id" : 1, "name" : "USA" }
{ "_id" : 2, "name" : "Canada" }
{ "_id" : 3, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : false } - Iteration 1
```json
{ "_id" : 1, "name" : "USA" }
{ "_id" : 2, "name" : "Canada" }
{ "_id" : 3, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : false } - Iteration 2
```json
{ "_id" : 1, "name" : "USA" }
{ "_id" : 2, "name" : "Canada" }
{ "_id" : 3, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```

### Pipeline
```json
[
	{
		"$match" : {
			"name" : "USA",
			"_id" : {
				"$gt" : 0
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : {
			"path" : "$cities",
			"preserveNullAndEmptyArrays" : true
		}
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json
{ "_id" : 1, "name" : "USA" }
```
### Options: { "allowDiskUse" : true } - Iteration 1
```json
{ "_id" : 1, "name" : "USA" }
```
### Options: { "allowDiskUse" : true } - Iteration 2
```json
{ "_id" : 1, "name" : "USA" }
```
### Options: { "allowDiskUse" : false } - Iteration 0
```json
{ "_id" : 1, "name" : "USA" }
```
### Options: { "allowDiskUse" : false } - Iteration 1
```json
{ "_id" : 1, "name" : "USA" }
```
### Options: { "allowDiskUse" : false } - Iteration 2
```json
{ "_id" : 1, "name" : "USA" }
```

### Pipeline
```json
[
	{
		"$match" : {
			"name" : "Canada",
			"nonExistentField" : {
				"$exists" : false
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : {
			"path" : "$cities",
			"preserveNullAndEmptyArrays" : true
		}
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json
{ "_id" : 2, "name" : "Canada" }
```
### Options: { "allowDiskUse" : true } - Iteration 1
```json
{ "_id" : 2, "name" : "Canada" }
```
### Options: { "allowDiskUse" : true } - Iteration 2
```json
{ "_id" : 2, "name" : "Canada" }
```
### Options: { "allowDiskUse" : false } - Iteration 0
```json
{ "_id" : 2, "name" : "Canada" }
```
### Options: { "allowDiskUse" : false } - Iteration 1
```json
{ "_id" : 2, "name" : "Canada" }
```
### Options: { "allowDiskUse" : false } - Iteration 2
```json
{ "_id" : 2, "name" : "Canada" }
```

### Pipeline
```json
[
	{
		"$match" : {
			"$and" : [
				{
					"name" : "France"
				},
				{
					"_id" : 3
				}
			]
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : {
			"path" : "$cities",
			"preserveNullAndEmptyArrays" : true
		}
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json
{ "_id" : 3, "name" : "France" }
```
### Options: { "allowDiskUse" : true } - Iteration 1
```json
{ "_id" : 3, "name" : "France" }
```
### Options: { "allowDiskUse" : true } - Iteration 2
```json
{ "_id" : 3, "name" : "France" }
```
### Options: { "allowDiskUse" : false } - Iteration 0
```json
{ "_id" : 3, "name" : "France" }
```
### Options: { "allowDiskUse" : false } - Iteration 1
```json
{ "_id" : 3, "name" : "France" }
```
### Options: { "allowDiskUse" : false } - Iteration 2
```json
{ "_id" : 3, "name" : "France" }
```

## 13. countries: [ { "_id" : 1, "name" : "USA" }, { "_id" : 2, "name" : "Canada" }, { "_id" : 3, "name" : "France" }, { "_id" : 4, "name" : "Romania" } ] - cities: [ { "_id" : 10, "countryId" : 1, "cityName" : "New York" }, { "_id" : 11, "countryId" : 1, "cityName" : "Los Angeles" }, { "_id" : 12, "countryId" : 2, "cityName" : "Toronto" }, { "_id" : 13, "countryId" : 3, "cityName" : "Paris" } ] - Indexes: [ ]

### Pipeline
```json
[
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : "$cities"
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```
### Options: { "allowDiskUse" : true } - Iteration 1
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```
### Options: { "allowDiskUse" : true } - Iteration 2
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```
### Options: { "allowDiskUse" : false } - Iteration 0
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```
### Options: { "allowDiskUse" : false } - Iteration 1
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```
### Options: { "allowDiskUse" : false } - Iteration 2
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```

### Pipeline
```json
[
	{
		"$match" : {
			"_id" : {
				"$gt" : 0
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : "$cities"
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```
### Options: { "allowDiskUse" : true } - Iteration 1
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```
### Options: { "allowDiskUse" : true } - Iteration 2
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```
### Options: { "allowDiskUse" : false } - Iteration 0
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```
### Options: { "allowDiskUse" : false } - Iteration 1
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```
### Options: { "allowDiskUse" : false } - Iteration 2
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```

### Pipeline
```json
[
	{
		"$match" : {
			"nonExistentField" : {
				"$exists" : false
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : "$cities"
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```
### Options: { "allowDiskUse" : true } - Iteration 1
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```
### Options: { "allowDiskUse" : true } - Iteration 2
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```
### Options: { "allowDiskUse" : false } - Iteration 0
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```
### Options: { "allowDiskUse" : false } - Iteration 1
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```
### Options: { "allowDiskUse" : false } - Iteration 2
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```

### Pipeline
```json
[
	{
		"$match" : {
			"name" : "USA",
			"_id" : {
				"$gt" : 0
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : "$cities"
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
```
### Options: { "allowDiskUse" : true } - Iteration 1
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
```
### Options: { "allowDiskUse" : true } - Iteration 2
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
```
### Options: { "allowDiskUse" : false } - Iteration 0
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
```
### Options: { "allowDiskUse" : false } - Iteration 1
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
```
### Options: { "allowDiskUse" : false } - Iteration 2
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
```

### Pipeline
```json
[
	{
		"$match" : {
			"name" : "Canada",
			"nonExistentField" : {
				"$exists" : false
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : "$cities"
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
```
### Options: { "allowDiskUse" : true } - Iteration 1
```json
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
```
### Options: { "allowDiskUse" : true } - Iteration 2
```json
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
```
### Options: { "allowDiskUse" : false } - Iteration 0
```json
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
```
### Options: { "allowDiskUse" : false } - Iteration 1
```json
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
```
### Options: { "allowDiskUse" : false } - Iteration 2
```json
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
```

### Pipeline
```json
[
	{
		"$match" : {
			"$and" : [
				{
					"name" : "France"
				},
				{
					"_id" : 3
				}
			]
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : "$cities"
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```
### Options: { "allowDiskUse" : true } - Iteration 1
```json
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```
### Options: { "allowDiskUse" : true } - Iteration 2
```json
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```
### Options: { "allowDiskUse" : false } - Iteration 0
```json
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```
### Options: { "allowDiskUse" : false } - Iteration 1
```json
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```
### Options: { "allowDiskUse" : false } - Iteration 2
```json
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```

### Pipeline
```json
[
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : {
			"path" : "$cities",
			"preserveNullAndEmptyArrays" : true
		}
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : true } - Iteration 1
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : true } - Iteration 2
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : false } - Iteration 0
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : false } - Iteration 1
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : false } - Iteration 2
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```

### Pipeline
```json
[
	{
		"$match" : {
			"_id" : {
				"$gt" : 0
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : {
			"path" : "$cities",
			"preserveNullAndEmptyArrays" : true
		}
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : true } - Iteration 1
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : true } - Iteration 2
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : false } - Iteration 0
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : false } - Iteration 1
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : false } - Iteration 2
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```

### Pipeline
```json
[
	{
		"$match" : {
			"nonExistentField" : {
				"$exists" : false
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : {
			"path" : "$cities",
			"preserveNullAndEmptyArrays" : true
		}
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : true } - Iteration 1
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : true } - Iteration 2
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : false } - Iteration 0
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : false } - Iteration 1
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : false } - Iteration 2
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```

### Pipeline
```json
[
	{
		"$match" : {
			"name" : "USA",
			"_id" : {
				"$gt" : 0
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : {
			"path" : "$cities",
			"preserveNullAndEmptyArrays" : true
		}
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
```
### Options: { "allowDiskUse" : true } - Iteration 1
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
```
### Options: { "allowDiskUse" : true } - Iteration 2
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
```
### Options: { "allowDiskUse" : false } - Iteration 0
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
```
### Options: { "allowDiskUse" : false } - Iteration 1
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
```
### Options: { "allowDiskUse" : false } - Iteration 2
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
```

### Pipeline
```json
[
	{
		"$match" : {
			"name" : "Canada",
			"nonExistentField" : {
				"$exists" : false
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : {
			"path" : "$cities",
			"preserveNullAndEmptyArrays" : true
		}
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
```
### Options: { "allowDiskUse" : true } - Iteration 1
```json
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
```
### Options: { "allowDiskUse" : true } - Iteration 2
```json
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
```
### Options: { "allowDiskUse" : false } - Iteration 0
```json
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
```
### Options: { "allowDiskUse" : false } - Iteration 1
```json
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
```
### Options: { "allowDiskUse" : false } - Iteration 2
```json
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
```

### Pipeline
```json
[
	{
		"$match" : {
			"$and" : [
				{
					"name" : "France"
				},
				{
					"_id" : 3
				}
			]
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : {
			"path" : "$cities",
			"preserveNullAndEmptyArrays" : true
		}
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```
### Options: { "allowDiskUse" : true } - Iteration 1
```json
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```
### Options: { "allowDiskUse" : true } - Iteration 2
```json
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```
### Options: { "allowDiskUse" : false } - Iteration 0
```json
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```
### Options: { "allowDiskUse" : false } - Iteration 1
```json
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```
### Options: { "allowDiskUse" : false } - Iteration 2
```json
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```

## 14. countries: [ { "_id" : 1, "name" : "USA" }, { "_id" : 2, "name" : "Canada" }, { "_id" : 3, "name" : "France" }, { "_id" : 4, "name" : "Romania" } ] - cities: [ { "_id" : 10, "countryId" : 1, "cityName" : "New York" }, { "_id" : 11, "countryId" : 1, "cityName" : "Los Angeles" }, { "_id" : 12, "countryId" : 2, "cityName" : "Toronto" }, { "_id" : 13, "countryId" : 3, "cityName" : "Paris" } ] - Indexes: [ { "collection" : "cities", "key" : { "countryId" : 1 } } ]

### Pipeline
```json
[
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : "$cities"
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```
### Options: { "allowDiskUse" : true } - Iteration 1
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```
### Options: { "allowDiskUse" : true } - Iteration 2
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```
### Options: { "allowDiskUse" : false } - Iteration 0
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```
### Options: { "allowDiskUse" : false } - Iteration 1
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```
### Options: { "allowDiskUse" : false } - Iteration 2
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```

### Pipeline
```json
[
	{
		"$match" : {
			"_id" : {
				"$gt" : 0
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : "$cities"
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```
### Options: { "allowDiskUse" : true } - Iteration 1
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```
### Options: { "allowDiskUse" : true } - Iteration 2
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```
### Options: { "allowDiskUse" : false } - Iteration 0
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```
### Options: { "allowDiskUse" : false } - Iteration 1
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```
### Options: { "allowDiskUse" : false } - Iteration 2
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```

### Pipeline
```json
[
	{
		"$match" : {
			"nonExistentField" : {
				"$exists" : false
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : "$cities"
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```
### Options: { "allowDiskUse" : true } - Iteration 1
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```
### Options: { "allowDiskUse" : true } - Iteration 2
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```
### Options: { "allowDiskUse" : false } - Iteration 0
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```
### Options: { "allowDiskUse" : false } - Iteration 1
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```
### Options: { "allowDiskUse" : false } - Iteration 2
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```

### Pipeline
```json
[
	{
		"$match" : {
			"name" : "USA",
			"_id" : {
				"$gt" : 0
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : "$cities"
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
```
### Options: { "allowDiskUse" : true } - Iteration 1
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
```
### Options: { "allowDiskUse" : true } - Iteration 2
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
```
### Options: { "allowDiskUse" : false } - Iteration 0
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
```
### Options: { "allowDiskUse" : false } - Iteration 1
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
```
### Options: { "allowDiskUse" : false } - Iteration 2
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
```

### Pipeline
```json
[
	{
		"$match" : {
			"name" : "Canada",
			"nonExistentField" : {
				"$exists" : false
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : "$cities"
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
```
### Options: { "allowDiskUse" : true } - Iteration 1
```json
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
```
### Options: { "allowDiskUse" : true } - Iteration 2
```json
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
```
### Options: { "allowDiskUse" : false } - Iteration 0
```json
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
```
### Options: { "allowDiskUse" : false } - Iteration 1
```json
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
```
### Options: { "allowDiskUse" : false } - Iteration 2
```json
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
```

### Pipeline
```json
[
	{
		"$match" : {
			"$and" : [
				{
					"name" : "France"
				},
				{
					"_id" : 3
				}
			]
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : "$cities"
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```
### Options: { "allowDiskUse" : true } - Iteration 1
```json
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```
### Options: { "allowDiskUse" : true } - Iteration 2
```json
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```
### Options: { "allowDiskUse" : false } - Iteration 0
```json
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```
### Options: { "allowDiskUse" : false } - Iteration 1
```json
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```
### Options: { "allowDiskUse" : false } - Iteration 2
```json
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```

### Pipeline
```json
[
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : {
			"path" : "$cities",
			"preserveNullAndEmptyArrays" : true
		}
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : true } - Iteration 1
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : true } - Iteration 2
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : false } - Iteration 0
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : false } - Iteration 1
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : false } - Iteration 2
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```

### Pipeline
```json
[
	{
		"$match" : {
			"_id" : {
				"$gt" : 0
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : {
			"path" : "$cities",
			"preserveNullAndEmptyArrays" : true
		}
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : true } - Iteration 1
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : true } - Iteration 2
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : false } - Iteration 0
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : false } - Iteration 1
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : false } - Iteration 2
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```

### Pipeline
```json
[
	{
		"$match" : {
			"nonExistentField" : {
				"$exists" : false
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : {
			"path" : "$cities",
			"preserveNullAndEmptyArrays" : true
		}
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : true } - Iteration 1
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : true } - Iteration 2
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : false } - Iteration 0
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : false } - Iteration 1
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : false } - Iteration 2
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```

### Pipeline
```json
[
	{
		"$match" : {
			"name" : "USA",
			"_id" : {
				"$gt" : 0
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : {
			"path" : "$cities",
			"preserveNullAndEmptyArrays" : true
		}
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
```
### Options: { "allowDiskUse" : true } - Iteration 1
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
```
### Options: { "allowDiskUse" : true } - Iteration 2
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
```
### Options: { "allowDiskUse" : false } - Iteration 0
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
```
### Options: { "allowDiskUse" : false } - Iteration 1
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
```
### Options: { "allowDiskUse" : false } - Iteration 2
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
```

### Pipeline
```json
[
	{
		"$match" : {
			"name" : "Canada",
			"nonExistentField" : {
				"$exists" : false
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : {
			"path" : "$cities",
			"preserveNullAndEmptyArrays" : true
		}
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
```
### Options: { "allowDiskUse" : true } - Iteration 1
```json
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
```
### Options: { "allowDiskUse" : true } - Iteration 2
```json
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
```
### Options: { "allowDiskUse" : false } - Iteration 0
```json
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
```
### Options: { "allowDiskUse" : false } - Iteration 1
```json
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
```
### Options: { "allowDiskUse" : false } - Iteration 2
```json
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
```

### Pipeline
```json
[
	{
		"$match" : {
			"$and" : [
				{
					"name" : "France"
				},
				{
					"_id" : 3
				}
			]
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : {
			"path" : "$cities",
			"preserveNullAndEmptyArrays" : true
		}
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```
### Options: { "allowDiskUse" : true } - Iteration 1
```json
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```
### Options: { "allowDiskUse" : true } - Iteration 2
```json
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```
### Options: { "allowDiskUse" : false } - Iteration 0
```json
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```
### Options: { "allowDiskUse" : false } - Iteration 1
```json
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```
### Options: { "allowDiskUse" : false } - Iteration 2
```json
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```

## 15. countries: [ { "_id" : 1, "name" : "USA" }, { "_id" : 2, "name" : "Canada" }, { "_id" : 3, "name" : "France" }, { "_id" : 4, "name" : "Romania" } ] - cities: [ { "_id" : 10, "countryId" : 1, "cityName" : "New York" }, { "_id" : 11, "countryId" : 1, "cityName" : "Los Angeles" }, { "_id" : 12, "countryId" : 2, "cityName" : "Toronto" }, { "_id" : 13, "countryId" : 3, "cityName" : "Paris" } ] - Indexes: [ { "collection" : "countries", "key" : { "name" : 1 } } ]

### Pipeline
```json
[
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : "$cities"
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```
### Options: { "allowDiskUse" : true } - Iteration 1
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```
### Options: { "allowDiskUse" : true } - Iteration 2
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```
### Options: { "allowDiskUse" : false } - Iteration 0
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```
### Options: { "allowDiskUse" : false } - Iteration 1
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```
### Options: { "allowDiskUse" : false } - Iteration 2
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```

### Pipeline
```json
[
	{
		"$match" : {
			"_id" : {
				"$gt" : 0
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : "$cities"
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```
### Options: { "allowDiskUse" : true } - Iteration 1
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```
### Options: { "allowDiskUse" : true } - Iteration 2
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```
### Options: { "allowDiskUse" : false } - Iteration 0
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```
### Options: { "allowDiskUse" : false } - Iteration 1
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```
### Options: { "allowDiskUse" : false } - Iteration 2
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```

### Pipeline
```json
[
	{
		"$match" : {
			"nonExistentField" : {
				"$exists" : false
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : "$cities"
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```
### Options: { "allowDiskUse" : true } - Iteration 1
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```
### Options: { "allowDiskUse" : true } - Iteration 2
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```
### Options: { "allowDiskUse" : false } - Iteration 0
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```
### Options: { "allowDiskUse" : false } - Iteration 1
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```
### Options: { "allowDiskUse" : false } - Iteration 2
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```

### Pipeline
```json
[
	{
		"$match" : {
			"name" : "USA",
			"_id" : {
				"$gt" : 0
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : "$cities"
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
```
### Options: { "allowDiskUse" : true } - Iteration 1
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
```
### Options: { "allowDiskUse" : true } - Iteration 2
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
```
### Options: { "allowDiskUse" : false } - Iteration 0
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
```
### Options: { "allowDiskUse" : false } - Iteration 1
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
```
### Options: { "allowDiskUse" : false } - Iteration 2
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
```

### Pipeline
```json
[
	{
		"$match" : {
			"name" : "Canada",
			"nonExistentField" : {
				"$exists" : false
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : "$cities"
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
```
### Options: { "allowDiskUse" : true } - Iteration 1
```json
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
```
### Options: { "allowDiskUse" : true } - Iteration 2
```json
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
```
### Options: { "allowDiskUse" : false } - Iteration 0
```json
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
```
### Options: { "allowDiskUse" : false } - Iteration 1
```json
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
```
### Options: { "allowDiskUse" : false } - Iteration 2
```json
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
```

### Pipeline
```json
[
	{
		"$match" : {
			"$and" : [
				{
					"name" : "France"
				},
				{
					"_id" : 3
				}
			]
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : "$cities"
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```
### Options: { "allowDiskUse" : true } - Iteration 1
```json
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```
### Options: { "allowDiskUse" : true } - Iteration 2
```json
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```
### Options: { "allowDiskUse" : false } - Iteration 0
```json
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```
### Options: { "allowDiskUse" : false } - Iteration 1
```json
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```
### Options: { "allowDiskUse" : false } - Iteration 2
```json
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```

### Pipeline
```json
[
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : {
			"path" : "$cities",
			"preserveNullAndEmptyArrays" : true
		}
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : true } - Iteration 1
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : true } - Iteration 2
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : false } - Iteration 0
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : false } - Iteration 1
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : false } - Iteration 2
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```

### Pipeline
```json
[
	{
		"$match" : {
			"_id" : {
				"$gt" : 0
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : {
			"path" : "$cities",
			"preserveNullAndEmptyArrays" : true
		}
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : true } - Iteration 1
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : true } - Iteration 2
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : false } - Iteration 0
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : false } - Iteration 1
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : false } - Iteration 2
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```

### Pipeline
```json
[
	{
		"$match" : {
			"nonExistentField" : {
				"$exists" : false
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : {
			"path" : "$cities",
			"preserveNullAndEmptyArrays" : true
		}
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : true } - Iteration 1
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : true } - Iteration 2
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : false } - Iteration 0
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : false } - Iteration 1
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : false } - Iteration 2
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```

### Pipeline
```json
[
	{
		"$match" : {
			"name" : "USA",
			"_id" : {
				"$gt" : 0
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : {
			"path" : "$cities",
			"preserveNullAndEmptyArrays" : true
		}
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
```
### Options: { "allowDiskUse" : true } - Iteration 1
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
```
### Options: { "allowDiskUse" : true } - Iteration 2
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
```
### Options: { "allowDiskUse" : false } - Iteration 0
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
```
### Options: { "allowDiskUse" : false } - Iteration 1
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
```
### Options: { "allowDiskUse" : false } - Iteration 2
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
```

### Pipeline
```json
[
	{
		"$match" : {
			"name" : "Canada",
			"nonExistentField" : {
				"$exists" : false
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : {
			"path" : "$cities",
			"preserveNullAndEmptyArrays" : true
		}
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
```
### Options: { "allowDiskUse" : true } - Iteration 1
```json
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
```
### Options: { "allowDiskUse" : true } - Iteration 2
```json
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
```
### Options: { "allowDiskUse" : false } - Iteration 0
```json
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
```
### Options: { "allowDiskUse" : false } - Iteration 1
```json
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
```
### Options: { "allowDiskUse" : false } - Iteration 2
```json
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
```

### Pipeline
```json
[
	{
		"$match" : {
			"$and" : [
				{
					"name" : "France"
				},
				{
					"_id" : 3
				}
			]
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : {
			"path" : "$cities",
			"preserveNullAndEmptyArrays" : true
		}
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```
### Options: { "allowDiskUse" : true } - Iteration 1
```json
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```
### Options: { "allowDiskUse" : true } - Iteration 2
```json
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```
### Options: { "allowDiskUse" : false } - Iteration 0
```json
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```
### Options: { "allowDiskUse" : false } - Iteration 1
```json
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```
### Options: { "allowDiskUse" : false } - Iteration 2
```json
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```

## 16. countries: [ { "_id" : 1, "name" : "USA" }, { "_id" : 2, "name" : "Canada" }, { "_id" : 3, "name" : "France" }, { "_id" : 4, "name" : "Romania" } ] - cities: [ { "_id" : 10, "countryId" : 1, "cityName" : "New York" }, { "_id" : 11, "countryId" : 1, "cityName" : "Los Angeles" }, { "_id" : 12, "countryId" : 2, "cityName" : "Toronto" }, { "_id" : 13, "countryId" : 3, "cityName" : "Paris" } ] - Indexes: [ { "collection" : "countries", "key" : { "_id" : 1 } } ]

### Pipeline
```json
[
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : "$cities"
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```
### Options: { "allowDiskUse" : true } - Iteration 1
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```
### Options: { "allowDiskUse" : true } - Iteration 2
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```
### Options: { "allowDiskUse" : false } - Iteration 0
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```
### Options: { "allowDiskUse" : false } - Iteration 1
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```
### Options: { "allowDiskUse" : false } - Iteration 2
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```

### Pipeline
```json
[
	{
		"$match" : {
			"_id" : {
				"$gt" : 0
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : "$cities"
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```
### Options: { "allowDiskUse" : true } - Iteration 1
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```
### Options: { "allowDiskUse" : true } - Iteration 2
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```
### Options: { "allowDiskUse" : false } - Iteration 0
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```
### Options: { "allowDiskUse" : false } - Iteration 1
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```
### Options: { "allowDiskUse" : false } - Iteration 2
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```

### Pipeline
```json
[
	{
		"$match" : {
			"nonExistentField" : {
				"$exists" : false
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : "$cities"
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```
### Options: { "allowDiskUse" : true } - Iteration 1
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```
### Options: { "allowDiskUse" : true } - Iteration 2
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```
### Options: { "allowDiskUse" : false } - Iteration 0
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```
### Options: { "allowDiskUse" : false } - Iteration 1
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```
### Options: { "allowDiskUse" : false } - Iteration 2
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```

### Pipeline
```json
[
	{
		"$match" : {
			"name" : "USA",
			"_id" : {
				"$gt" : 0
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : "$cities"
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
```
### Options: { "allowDiskUse" : true } - Iteration 1
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
```
### Options: { "allowDiskUse" : true } - Iteration 2
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
```
### Options: { "allowDiskUse" : false } - Iteration 0
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
```
### Options: { "allowDiskUse" : false } - Iteration 1
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
```
### Options: { "allowDiskUse" : false } - Iteration 2
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
```

### Pipeline
```json
[
	{
		"$match" : {
			"name" : "Canada",
			"nonExistentField" : {
				"$exists" : false
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : "$cities"
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
```
### Options: { "allowDiskUse" : true } - Iteration 1
```json
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
```
### Options: { "allowDiskUse" : true } - Iteration 2
```json
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
```
### Options: { "allowDiskUse" : false } - Iteration 0
```json
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
```
### Options: { "allowDiskUse" : false } - Iteration 1
```json
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
```
### Options: { "allowDiskUse" : false } - Iteration 2
```json
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
```

### Pipeline
```json
[
	{
		"$match" : {
			"$and" : [
				{
					"name" : "France"
				},
				{
					"_id" : 3
				}
			]
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : "$cities"
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```
### Options: { "allowDiskUse" : true } - Iteration 1
```json
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```
### Options: { "allowDiskUse" : true } - Iteration 2
```json
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```
### Options: { "allowDiskUse" : false } - Iteration 0
```json
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```
### Options: { "allowDiskUse" : false } - Iteration 1
```json
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```
### Options: { "allowDiskUse" : false } - Iteration 2
```json
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```

### Pipeline
```json
[
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : {
			"path" : "$cities",
			"preserveNullAndEmptyArrays" : true
		}
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : true } - Iteration 1
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : true } - Iteration 2
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : false } - Iteration 0
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : false } - Iteration 1
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : false } - Iteration 2
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```

### Pipeline
```json
[
	{
		"$match" : {
			"_id" : {
				"$gt" : 0
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : {
			"path" : "$cities",
			"preserveNullAndEmptyArrays" : true
		}
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : true } - Iteration 1
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : true } - Iteration 2
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : false } - Iteration 0
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : false } - Iteration 1
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : false } - Iteration 2
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```

### Pipeline
```json
[
	{
		"$match" : {
			"nonExistentField" : {
				"$exists" : false
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : {
			"path" : "$cities",
			"preserveNullAndEmptyArrays" : true
		}
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : true } - Iteration 1
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : true } - Iteration 2
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : false } - Iteration 0
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : false } - Iteration 1
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```
### Options: { "allowDiskUse" : false } - Iteration 2
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
{ "_id" : 4, "name" : "Romania" }
```

### Pipeline
```json
[
	{
		"$match" : {
			"name" : "USA",
			"_id" : {
				"$gt" : 0
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : {
			"path" : "$cities",
			"preserveNullAndEmptyArrays" : true
		}
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
```
### Options: { "allowDiskUse" : true } - Iteration 1
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
```
### Options: { "allowDiskUse" : true } - Iteration 2
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
```
### Options: { "allowDiskUse" : false } - Iteration 0
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
```
### Options: { "allowDiskUse" : false } - Iteration 1
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
```
### Options: { "allowDiskUse" : false } - Iteration 2
```json
{ "_id" : 1, "cities" : { "_id" : 10, "cityName" : "New York", "countryId" : 1 }, "name" : "USA" }
{ "_id" : 1, "cities" : { "_id" : 11, "cityName" : "Los Angeles", "countryId" : 1 }, "name" : "USA" }
```

### Pipeline
```json
[
	{
		"$match" : {
			"name" : "Canada",
			"nonExistentField" : {
				"$exists" : false
			}
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : {
			"path" : "$cities",
			"preserveNullAndEmptyArrays" : true
		}
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
```
### Options: { "allowDiskUse" : true } - Iteration 1
```json
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
```
### Options: { "allowDiskUse" : true } - Iteration 2
```json
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
```
### Options: { "allowDiskUse" : false } - Iteration 0
```json
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
```
### Options: { "allowDiskUse" : false } - Iteration 1
```json
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
```
### Options: { "allowDiskUse" : false } - Iteration 2
```json
{ "_id" : 2, "cities" : { "_id" : 12, "cityName" : "Toronto", "countryId" : 2 }, "name" : "Canada" }
```

### Pipeline
```json
[
	{
		"$match" : {
			"$and" : [
				{
					"name" : "France"
				},
				{
					"_id" : 3
				}
			]
		}
	},
	{
		"$lookup" : {
			"from" : "cities",
			"localField" : "_id",
			"foreignField" : "countryId",
			"as" : "cities"
		}
	},
	{
		"$unwind" : {
			"path" : "$cities",
			"preserveNullAndEmptyArrays" : true
		}
	}
]
```
### Options: { "allowDiskUse" : true } - Iteration 0
```json
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```
### Options: { "allowDiskUse" : true } - Iteration 1
```json
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```
### Options: { "allowDiskUse" : true } - Iteration 2
```json
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```
### Options: { "allowDiskUse" : false } - Iteration 0
```json
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```
### Options: { "allowDiskUse" : false } - Iteration 1
```json
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```
### Options: { "allowDiskUse" : false } - Iteration 2
```json
{ "_id" : 3, "cities" : { "_id" : 13, "cityName" : "Paris", "countryId" : 3 }, "name" : "France" }
```



[jsTest] ----
[jsTest] Ran 1152 queries
[jsTest] ----

