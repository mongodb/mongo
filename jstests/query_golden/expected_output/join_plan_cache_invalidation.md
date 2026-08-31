## Cache invalidation NOT expected for those scenarios:


### No DDL
```json
[ ]
```
> [!INFO]
> As expected, the cache entry was correctly NOT invalidated.


### Normal insert
```json
[
	{
		"insert" : "base_coll",
		"documents" : [
			{
				"a" : 10
			}
		]
	}
]
```
> [!INFO]
> As expected, the cache entry was correctly NOT invalidated.


### Creating an unrelated collection
```json
[ { "create" : "baz" } ]
```
> [!INFO]
> As expected, the cache entry was correctly NOT invalidated.


### Creating an index on an unrelated collection
```json
[
	{
		"createIndexes" : "unrelated",
		"indexes" : [
			{
				"name" : "d_1",
				"key" : {
					"d" : 1
				}
			}
		]
	}
]
```
> [!INFO]
> As expected, the cache entry was correctly NOT invalidated.


### Creating a new unrelated index on the base collection
```json
[
	{
		"createIndexes" : "base_coll",
		"indexes" : [
			{
				"name" : "d_1",
				"key" : {
					"d" : 1
				}
			}
		]
	}
]
```
> [!INFO]
> As expected, the cache entry was correctly NOT invalidated.


### Creating a new unrelated index on the lookup collection
```json
[
	{
		"createIndexes" : "lookup_coll",
		"indexes" : [
			{
				"name" : "d_1",
				"key" : {
					"d" : 1
				}
			}
		]
	}
]
```
> [!INFO]
> As expected, the cache entry was correctly NOT invalidated.


### Unhiding an unrelated index on the base collection
```json
[
	{
		"collMod" : "base_coll",
		"index" : {
			"name" : "hidden_idx_1",
			"hidden" : false
		}
	}
]
```
> [!INFO]
> As expected, the cache entry was correctly NOT invalidated.


### Unhiding an unrelated index on the lookup collection
```json
[
	{
		"collMod" : "lookup_coll",
		"index" : {
			"name" : "hidden_idx_1",
			"hidden" : false
		}
	}
]
```
> [!INFO]
> As expected, the cache entry was correctly NOT invalidated.


### Dropping an unrelated collection
```json
[ { "drop" : "unrelated" } ]
```
> [!INFO]
> As expected, the cache entry was correctly NOT invalidated.


### Dropping an unrelated index on the base collection
```json
[ { "dropIndexes" : "base_coll", "index" : "unrelated_field_1" } ]
```
> [!INFO]
> As expected, the cache entry was correctly NOT invalidated.


### Dropping an unrelated index on the join collection
```json
[ { "dropIndexes" : "lookup_coll", "index" : "unrelated_field_1" } ]
```
> [!INFO]
> As expected, the cache entry was correctly NOT invalidated.


### Dropping an unrelated index on the view's base collection
```json
[ { "dropIndexes" : "base_coll", "index" : "unrelated_field_1" } ]
```
> [!INFO]
> As expected, the cache entry was correctly NOT invalidated.


### Dropping and recreating an index using exactly the same definition
```json
[
	{
		"dropIndexes" : "base_coll",
		"index" : "a_1"
	},
	{
		"createIndexes" : "base_coll",
		"indexes" : [
			{
				"name" : "a_1",
				"key" : {
					"a" : 1
				}
			}
		]
	}
]
```
> [!INFO]
> As expected, the cache entry was correctly NOT invalidated.
## Cache invalidation expected for those scenarios:


### Dropping the database
```json
[ { "dropDatabase" : 1 } ]
```
> [!INFO]
> Cache did not kick in at all


### Dropping the base collection (without recreating it)
```json
[ { "drop" : "base_coll" } ]
```
> [!INFO]
> Cache did not kick in at all


### Dropping the $lookup collection (without recreating it)
```json
[ { "drop" : "lookup_coll" } ]
```
> [!INFO]
> Cache did not kick in at all


### Dropping and recreating the base collection
```json
[
	{
		"drop" : "base_coll"
	},
	{
		"create" : "base_coll"
	},
	{
		"createIndexes" : "base_coll",
		"indexes" : [
			{
				"name" : "a_1",
				"key" : {
					"a" : 1
				}
			}
		]
	}
]
```
> [!INFO]
> As expected, the cache entry was correctly invalidated.


### Dropping and recreating the lookup collection
```json
[
	{
		"drop" : "lookup_coll"
	},
	{
		"create" : "lookup_coll"
	},
	{
		"createIndexes" : "lookup_coll",
		"indexes" : [
			{
				"name" : "a_1",
				"key" : {
					"a" : 1
				}
			}
		]
	}
]
```
> [!INFO]
> As expected, the cache entry was correctly invalidated.


### Renaming the base collection
```json
[
	{
		"renameCollection" : "test.base_coll",
		"to" : "test.base_coll_renamed"
	}
]
```
> [!INFO]
> Cache did not kick in at all


### Renaming the base collection and creating a new one with the same name
```json
[
	{
		"renameCollection" : "test.base_coll",
		"to" : "test.base_coll_renamed"
	},
	{
		"create" : "base_coll"
	},
	{
		"createIndexes" : "base_coll",
		"indexes" : [
			{
				"name" : "a_1",
				"key" : {
					"a" : 1
				}
			}
		]
	}
]
```
> [!INFO]
> As expected, the cache entry was correctly invalidated.


### Dropping the join index on the base collection
```json
[ { "dropIndexes" : "base_coll", "index" : "a_1" } ]
```
> [!INFO]
> Cache did not kick in at all


### Dropping the join index on the lookup collection
```json
[ { "dropIndexes" : "lookup_coll", "index" : "a_1" } ]
```
> [!INFO]
> Cache did not kick in at all


### Dropping the index on the base collection's single-table predicate
```json
[ { "dropIndexes" : "base_coll", "index" : "single_table_predicate_1" } ]
```
> [!INFO]
> As expected, the cache entry was correctly invalidated.


### Dropping the index on the $lookup collection's single-table predicate
```json
[
	{
		"dropIndexes" : "lookup_coll",
		"index" : "single_table_predicate_1"
	}
]
```
> [!INFO]
> As expected, the cache entry was correctly invalidated.


### Hiding the join index on the base collection
```json
[
	{
		"collMod" : "base_coll",
		"index" : {
			"name" : "a_1",
			"hidden" : true
		}
	}
]
```
> [!INFO]
> Cache did not kick in at all


### Hiding the join index on the lookup collection
```json
[
	{
		"collMod" : "lookup_coll",
		"index" : {
			"name" : "a_1",
			"hidden" : true
		}
	}
]
```
> [!INFO]
> Cache did not kick in at all


### TODO(SERVER-134231): Hiding the index on the base collection's single-table predicate
```json
[
	{
		"collMod" : "base_coll",
		"index" : {
			"name" : "single_table_predicate_1",
			"hidden" : true
		}
	}
]
```
> [!WARNING]
> Cache entry was expected to be invalidated but was not!


### TODO(SERVER-134231) Hiding the index on the $lookup collection's single-table predicate
```json
[
	{
		"collMod" : "lookup_coll",
		"index" : {
			"name" : "single_table_predicate_1",
			"hidden" : true
		}
	}
]
```
> [!WARNING]
> Cache entry was expected to be invalidated but was not!


### Unhiding a potentially useful index on the base collection
```json
[
	{
		"collMod" : "base_coll",
		"index" : {
			"name" : "single_table_predicate_1_hidden_idx_1",
			"hidden" : false
		}
	}
]
```
> [!INFO]
> As expected, the cache entry was correctly invalidated.


### Unhiding a potentially useful index on the lookup collection
```json
[
	{
		"collMod" : "lookup_coll",
		"index" : {
			"name" : "single_table_predicate_1_hidden_idx_1",
			"hidden" : false
		}
	}
]
```
> [!INFO]
> As expected, the cache entry was correctly invalidated.


### Creating a new index potentially useful for joins on the base collection
```json
[
	{
		"createIndexes" : "base_coll",
		"indexes" : [
			{
				"name" : "a_1_b_1",
				"key" : {
					"a" : 1,
					"b" : 1
				}
			}
		]
	}
]
```
> [!INFO]
> As expected, the cache entry was correctly invalidated.


### Creating a new index potentially useful for joins on the lookup collection
```json
[
	{
		"createIndexes" : "lookup_coll",
		"indexes" : [
			{
				"name" : "a_1_b_1",
				"key" : {
					"a" : 1,
					"b" : 1
				}
			}
		]
	}
]
```
> [!INFO]
> As expected, the cache entry was correctly invalidated.


### Creating a new index potentially useful for single-table predicate on the base collection
```json
[
	{
		"createIndexes" : "base_coll",
		"indexes" : [
			{
				"name" : "single_table_predicate_1_x_1",
				"key" : {
					"single_table_predicate" : 1,
					"x" : 1
				}
			}
		]
	}
]
```
> [!INFO]
> As expected, the cache entry was correctly invalidated.


### Creating a new index potentially useful for single-table predicate on the lookup collection
```json
[
	{
		"createIndexes" : "lookup_coll",
		"indexes" : [
			{
				"name" : "single_table_predicate_1_x_1",
				"key" : {
					"single_table_predicate" : 1,
					"x" : 1
				}
			}
		]
	}
]
```
> [!INFO]
> As expected, the cache entry was correctly invalidated.


### Creating a new index potentially useful for the single-table predicate on the base view
```json
[
	{
		"createIndexes" : "base_coll",
		"indexes" : [
			{
				"name" : "single_table_view_predicate_1_x_1",
				"key" : {
					"single_table_view_predicate" : 1,
					"x" : 1
				}
			}
		]
	}
]
```
> [!INFO]
> As expected, the cache entry was correctly invalidated.


### Making the join index multikey in the base collection
```json
[
	{
		"insert" : "base_coll",
		"documents" : [
			{
				"a" : [
					1,
					2
				]
			}
		]
	}
]
```
> [!INFO]
> Cache did not kick in at all


### Making the join index multikey in the lookup collection
```json
[
	{
		"insert" : "lookup_coll",
		"documents" : [
			{
				"a" : [
					1,
					2
				]
			}
		]
	}
]
```
> [!INFO]
> Cache did not kick in at all


### TODO(SERVER-130790): Making the single-table predicate index multikey
```json
[
	{
		"insert" : "base_coll",
		"documents" : [
			{
				"single_table_predicate" : [
					1,
					2
				]
			}
		]
	},
	{
		"insert" : "lookup_coll",
		"documents" : [
			{
				"single_table_predicate" : [
					1,
					2
				]
			}
		]
	}
]
```
> [!WARNING]
> Cache entry was expected to be invalidated but was not!


### Making an index unique on the base collection
```json
[
	{
		"collMod" : "base_coll",
		"index" : {
			"name" : "a_1",
			"prepareUnique" : true
		}
	},
	{
		"collMod" : "base_coll",
		"index" : {
			"name" : "a_1",
			"unique" : true
		}
	}
]
```
> [!INFO]
> As expected, the cache entry was correctly invalidated.


### Making an index unique on the lookup collection
```json
[
	{
		"collMod" : "lookup_coll",
		"index" : {
			"name" : "a_1",
			"prepareUnique" : true
		}
	},
	{
		"collMod" : "lookup_coll",
		"index" : {
			"name" : "a_1",
			"unique" : true
		}
	}
]
```
> [!INFO]
> As expected, the cache entry was correctly invalidated.


### Changing the definition of the view used as base collection
```json
[
	{
		"collMod" : "base_v",
		"viewOn" : "lookup_coll",
		"pipeline" : [ ]
	}
]
```
> [!INFO]
> As expected, the cache entry was correctly invalidated.


### Dropping the join index on the view's base collection
```json
[ { "dropIndexes" : "base_coll", "index" : "a_1" } ]
```
> [!INFO]
> Cache did not kick in at all


### Dropping the index on the view's base collection's single-table predicate
```json
[ { "dropIndexes" : "base_coll", "index" : "single_table_predicate_1" } ]
```
> [!INFO]
> As expected, the cache entry was correctly invalidated.


### Dropping the index on the view's single-table predicate
```json
[
	{
		"dropIndexes" : "base_coll",
		"index" : "single_table_view_predicate_1"
	}
]
```
> [!INFO]
> As expected, the cache entry was correctly invalidated.


### Dropping and recreating an index under a different name
```json
[
	{
		"dropIndexes" : "base_coll",
		"index" : "a_1"
	},
	{
		"createIndexes" : "base_coll",
		"indexes" : [
			{
				"name" : "a_new_1",
				"key" : {
					"a" : 1
				}
			}
		]
	}
]
```
> [!INFO]
> As expected, the cache entry was correctly invalidated.


### Dropping and recreating an index with a different keyPattern (different field)
```json
[
	{
		"dropIndexes" : "base_coll",
		"index" : "a_1"
	},
	{
		"createIndexes" : "base_coll",
		"indexes" : [
			{
				"name" : "a_1",
				"key" : {
					"z" : 1
				}
			}
		]
	}
]
```
> [!INFO]
> Cache did not kick in at all


### Dropping and recreating an index with a different keyPattern (different field list)
```json
[
	{
		"dropIndexes" : "base_coll",
		"index" : "a_1"
	},
	{
		"createIndexes" : "base_coll",
		"indexes" : [
			{
				"name" : "a_1",
				"key" : {
					"a" : 1,
					"b" : 1
				}
			}
		]
	}
]
```
> [!INFO]
> As expected, the cache entry was correctly invalidated.


### Dropping and recreating an index with a different keyPattern (different index direction)
```json
[
	{
		"dropIndexes" : "base_coll",
		"index" : "a_1"
	},
	{
		"createIndexes" : "base_coll",
		"indexes" : [
			{
				"name" : "a_1",
				"key" : {
					"a" : -1
				}
			}
		]
	}
]
```
> [!INFO]
> As expected, the cache entry was correctly invalidated.


### Dropping and recreating an index with a different keyPattern (different uniqueness)
```json
[
	{
		"dropIndexes" : "base_coll",
		"index" : "a_1"
	},
	{
		"createIndexes" : "base_coll",
		"indexes" : [
			{
				"name" : "a_1",
				"key" : {
					"a" : -1
				},
				"unique" : true
			}
		]
	}
]
```
> [!INFO]
> As expected, the cache entry was correctly invalidated.


### Dropping and recreating an index with a different keyPattern (different partialFilterExpression)
```json
[
	{
		"dropIndexes" : "base_coll",
		"index" : "a_1"
	},
	{
		"createIndexes" : "base_coll",
		"indexes" : [
			{
				"name" : "a_1",
				"key" : {
					"a" : -1
				},
				"partialFilterExpression" : {
					"a" : 0
				}
			}
		]
	}
]
```
> [!INFO]
> Cache did not kick in at all


### TODO(SERVER-134231) Dropping and recreating an index with a different keyPattern (different sparseness)
```json
[
	{
		"dropIndexes" : "base_coll",
		"index" : "a_1"
	},
	{
		"createIndexes" : "base_coll",
		"indexes" : [
			{
				"name" : "a_1",
				"key" : {
					"a" : -1
				},
				"sparse" : true
			}
		]
	}
]
```
> [!WARNING]
> Cache entry was expected to be invalidated but was not!


### Dropping and recreating an index with a different keyPattern (different hidden state)
```json
[
	{
		"dropIndexes" : "base_coll",
		"index" : "a_1"
	},
	{
		"createIndexes" : "base_coll",
		"indexes" : [
			{
				"name" : "a_1",
				"key" : {
					"a" : -1
				},
				"hidden" : true
			}
		]
	}
]
```
> [!INFO]
> Cache did not kick in at all


### TODO(SERVER-134231): Dropping and recreating an index with a different keyPattern (different collation)
```json
[
	{
		"dropIndexes" : "base_coll",
		"index" : "a_1"
	},
	{
		"createIndexes" : "base_coll",
		"indexes" : [
			{
				"name" : "a_1",
				"key" : {
					"a" : -1
				},
				"collation" : {
					"locale" : "fr"
				}
			}
		]
	}
]
```
> [!WARNING]
> Cache entry was expected to be invalidated but was not!


### Dropping and recreating an index with a different keyPattern (different index type)
```json
[
	{
		"dropIndexes" : "base_coll",
		"index" : "a_1"
	},
	{
		"createIndexes" : "base_coll",
		"indexes" : [
			{
				"name" : "a_1",
				"key" : {
					"a" : "hashed"
				}
			}
		]
	}
]
```
> [!INFO]
> Cache did not kick in at all
