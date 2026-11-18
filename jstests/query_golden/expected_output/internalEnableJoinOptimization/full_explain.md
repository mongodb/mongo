## 1. Query
```json
[
	{
		"$match" : {
			"status" : "shipped",
			"total" : {
				"$gte" : 100
			}
		}
	},
	{
		"$lookup" : {
			"from" : "customers",
			"localField" : "customerId",
			"foreignField" : "_id",
			"as" : "cust"
		}
	},
	{
		"$unwind" : "$cust"
	},
	{
		"$lookup" : {
			"from" : "products",
			"localField" : "productId",
			"foreignField" : "_id",
			"as" : "prod"
		}
	},
	{
		"$unwind" : "$prod"
	},
	{
		"$lookup" : {
			"from" : "suppliers",
			"localField" : "prod.supplierCode",
			"foreignField" : "code",
			"as" : "supp"
		}
	},
	{
		"$unwind" : "$supp"
	},
	{
		"$match" : {
			"cust.tier" : "gold",
			"cust.active" : true,
			"prod.category" : {
				"$in" : [
					"books",
					"toys"
				]
			},
			"supp.region" : "EMEA"
		}
	},
	{
		"$group" : {
			"_id" : "$cust.country",
			"revenue" : {
				"$sum" : "$total"
			}
		}
	}
]
```
Results:
```json
[ { "_id" : "UK", "revenue" : 1044 } ]
```
Explain values that change between runs or are covered by other tests are replaced with `"<normalized>"`.

## 2. queryPlanner verbosity
```json
{
	"namespace" : "shop.orders",
	"optimizationTimeMillis" : "<normalized>",
	"optimizationTimeMicros" : "<normalized>",
	"optimizedPipeline" : true,
	"joinOptimizer" : {
		"planCacheKey" : "<normalized>",
		"fallbackReason" : "unsupportedStage"
	},
	"ceSamplingMetadata" : {
		"shop.customers" : {
			"sampleSource" : "onTheFly",
			"sampleTechnique" : "strides",
			"sampleRequestedDocCount" : 1063,
			"sampleDocCount" : 100,
			"sampleMemorySizeBytes" : 8762,
			"sampleCreatedAt" : "<normalized>"
		},
		"shop.orders" : {
			"sampleSource" : "onTheFly",
			"sampleTechnique" : "strides",
			"sampleRequestedDocCount" : 1063,
			"sampleDocCount" : 1000,
			"sampleMemorySizeBytes" : 117158,
			"sampleCreatedAt" : "<normalized>"
		},
		"shop.products" : {
			"sampleSource" : "onTheFly",
			"sampleTechnique" : "strides",
			"sampleRequestedDocCount" : 1063,
			"sampleDocCount" : 50,
			"sampleMemorySizeBytes" : 4398,
			"sampleCreatedAt" : "<normalized>"
		},
		"shop.suppliers" : {
			"sampleSource" : "onTheFly",
			"sampleTechnique" : "strides",
			"sampleRequestedDocCount" : 1063,
			"sampleDocCount" : 6,
			"sampleMemorySizeBytes" : 488,
			"sampleCreatedAt" : "<normalized>"
		}
	},
	"maxIndexedOrSolutionsReached" : false,
	"maxIndexedAndSolutionsReached" : false,
	"maxScansToExplodeReached" : false,
	"prunedSimilarIndexes" : false,
	"winningPlan" : {
		"isCached" : false,
		"usedJoinOptimization" : true,
		"queryPlan" : {
			"stage" : "GROUP",
			"planNodeId" : 11,
			"inputStage" : {
				"stage" : "NESTED_LOOP_JOIN_EMBEDDING",
				"planNodeId" : 10,
				"leftEmbeddingField" : "none",
				"rightEmbeddingField" : "supp",
				"joinPredicates" : [
					"prod.supplierCode = code"
				],
				"costEstimate" : "<normalized>",
				"cardinalityEstimate" : "<normalized>",
				"estimatesMetadata" : {
					"ceSource" : "Sampling"
				},
				"inputStages" : [
					{
						"stage" : "HASH_JOIN_EMBEDDING",
						"planNodeId" : 8,
						"leftEmbeddingField" : "none",
						"rightEmbeddingField" : "prod",
						"joinPredicates" : [
							"productId = _id"
						],
						"costEstimate" : "<normalized>",
						"cardinalityEstimate" : "<normalized>",
						"estimatesMetadata" : {
							"ceSource" : "Metadata"
						},
						"inputStages" : [
							{
								"stage" : "INDEXED_NESTED_LOOP_JOIN_EMBEDDING",
								"planNodeId" : 6,
								"leftEmbeddingField" : "cust",
								"rightEmbeddingField" : "none",
								"joinPredicates" : [
									"_id = customerId"
								],
								"probePredicates" : [
									"_id = customerId"
								],
								"costEstimate" : "<normalized>",
								"cardinalityEstimate" : "<normalized>",
								"estimatesMetadata" : {
									"ceSource" : "Metadata"
								},
								"inputStages" : [
									{
										"stage" : "FETCH",
										"planNodeId" : 2,
										"nss" : "shop.customers",
										"filter" : {
											"active" : {
												"$eq" : true
											}
										},
										"costEstimate" : "<normalized>",
										"cardinalityEstimate" : "<normalized>",
										"estimatesMetadata" : {
											"ceSource" : "Metadata"
										},
										"inputStage" : {
											"stage" : "IXSCAN",
											"planNodeId" : 1,
											"nss" : "shop.customers",
											"keyPattern" : {
												"tier" : 1
											},
											"indexName" : "tier_1",
											"isMultiKey" : false,
											"multiKeyPaths" : {
												"tier" : [ ]
											},
											"isUnique" : false,
											"isSparse" : false,
											"isPartial" : false,
											"indexVersion" : 2,
											"direction" : "forward",
											"indexBounds" : {
												"tier" : [
													"[\"gold\", \"gold\"]"
												]
											}
										}
									},
									{
										"stage" : "PROJECTION_DEFAULT",
										"planNodeId" : 5,
										"transformBy" : {
											"customerId" : true,
											"productId" : true,
											"total" : true,
											"_id" : false
										},
										"inputStage" : {
											"stage" : "FETCH",
											"planNodeId" : 4,
											"nss" : "shop.orders",
											"filter" : {
												"$and" : [
													{
														"status" : {
															"$eq" : "shipped"
														}
													},
													{
														"total" : {
															"$gte" : 100
														}
													}
												]
											},
											"inputStage" : {
												"stage" : "INDEX_PROBE_NODE",
												"planNodeId" : 3,
												"nss" : "shop.orders",
												"keyPattern" : {
													"customerId" : 1
												},
												"indexName" : "customerId_1",
												"isMultiKey" : false,
												"isUnique" : false,
												"isSparse" : false,
												"isPartial" : false,
												"indexVersion" : 2
											}
										}
									}
								]
							},
							{
								"stage" : "COLLSCAN",
								"planNodeId" : 7,
								"nss" : "shop.products",
								"direction" : "forward",
								"filter" : {
									"category" : {
										"$in" : [
											"books",
											"toys"
										]
									}
								},
								"costEstimate" : "<normalized>",
								"cardinalityEstimate" : "<normalized>",
								"estimatesMetadata" : {
									"ceSource" : "Metadata"
								}
							}
						]
					},
					{
						"stage" : "COLLSCAN",
						"planNodeId" : 9,
						"nss" : "shop.suppliers",
						"direction" : "forward",
						"filter" : {
							"region" : {
								"$eq" : "EMEA"
							}
						},
						"costEstimate" : "<normalized>",
						"cardinalityEstimate" : "<normalized>",
						"estimatesMetadata" : {
							"ceSource" : "Metadata"
						}
					}
				]
			}
		},
		"slotBasedPlan" : "<normalized>"
	},
	"rejectedPlans" : [
		{
			"isCached" : false,
			"usedJoinOptimization" : true,
			"queryPlan" : {
				"stage" : "NESTED_LOOP_JOIN_EMBEDDING",
				"planNodeId" : 9,
				"leftEmbeddingField" : "none",
				"rightEmbeddingField" : "supp",
				"joinPredicates" : [
					"prod.supplierCode = code"
				],
				"costEstimate" : "<normalized>",
				"cardinalityEstimate" : "<normalized>",
				"estimatesMetadata" : {
					"ceSource" : "Sampling"
				},
				"inputStages" : [
					{
						"stage" : "HASH_JOIN_EMBEDDING",
						"planNodeId" : 7,
						"leftEmbeddingField" : "none",
						"rightEmbeddingField" : "prod",
						"joinPredicates" : [
							"productId = _id"
						],
						"costEstimate" : "<normalized>",
						"cardinalityEstimate" : "<normalized>",
						"estimatesMetadata" : {
							"ceSource" : "Metadata"
						},
						"inputStages" : [
							{
								"stage" : "INDEXED_NESTED_LOOP_JOIN_EMBEDDING",
								"planNodeId" : 5,
								"leftEmbeddingField" : "none",
								"rightEmbeddingField" : "cust",
								"joinPredicates" : [
									"customerId = _id"
								],
								"probePredicates" : [
									"customerId = _id"
								],
								"costEstimate" : "<normalized>",
								"cardinalityEstimate" : "<normalized>",
								"estimatesMetadata" : {
									"ceSource" : "Metadata"
								},
								"inputStages" : [
									{
										"stage" : "PROJECTION_SIMPLE",
										"planNodeId" : 2,
										"transformBy" : {
											"customerId" : true,
											"productId" : true,
											"total" : true,
											"_id" : false
										},
										"costEstimate" : "<normalized>",
										"cardinalityEstimate" : "<normalized>",
										"estimatesMetadata" : {
											"ceSource" : "Metadata"
										},
										"inputStage" : {
											"stage" : "COLLSCAN",
											"planNodeId" : 1,
											"nss" : "shop.orders",
											"direction" : "forward",
											"filter" : {
												"$and" : [
													{
														"status" : {
															"$eq" : "shipped"
														}
													},
													{
														"total" : {
															"$gte" : 100
														}
													}
												]
											}
										}
									},
									{
										"stage" : "FETCH",
										"planNodeId" : 4,
										"nss" : "shop.customers",
										"filter" : {
											"$and" : [
												{
													"active" : {
														"$eq" : true
													}
												},
												{
													"tier" : {
														"$eq" : "gold"
													}
												}
											]
										},
										"inputStage" : {
											"stage" : "INDEX_PROBE_NODE",
											"planNodeId" : 3,
											"nss" : "shop.customers",
											"keyPattern" : {
												"_id" : 1
											},
											"indexName" : "_id_",
											"isMultiKey" : false,
											"isUnique" : true,
											"isSparse" : false,
											"isPartial" : false,
											"indexVersion" : 2
										}
									}
								]
							},
							{
								"stage" : "COLLSCAN",
								"planNodeId" : 6,
								"nss" : "shop.products",
								"direction" : "forward",
								"filter" : {
									"category" : {
										"$in" : [
											"books",
											"toys"
										]
									}
								},
								"costEstimate" : "<normalized>",
								"cardinalityEstimate" : "<normalized>",
								"estimatesMetadata" : {
									"ceSource" : "Metadata"
								}
							}
						]
					},
					{
						"stage" : "COLLSCAN",
						"planNodeId" : 8,
						"nss" : "shop.suppliers",
						"direction" : "forward",
						"filter" : {
							"region" : {
								"$eq" : "EMEA"
							}
						},
						"costEstimate" : "<normalized>",
						"cardinalityEstimate" : "<normalized>",
						"estimatesMetadata" : {
							"ceSource" : "Metadata"
						}
					}
				]
			},
			"slotBasedPlan" : "<normalized>"
		}
	]
}
```
## 3. queryPlanner verbosity with enableDetailedJoinExplain
```json
{
	"namespace" : "shop.orders",
	"optimizationTimeMillis" : "<normalized>",
	"optimizationTimeMicros" : "<normalized>",
	"optimizedPipeline" : true,
	"joinOptimizer" : {
		"planCacheKey" : "<normalized>",
		"fallbackReason" : "unsupportedStage",
		"metrics" : {
			"joinModelingTimeMicros" : "<normalized>",
			"sbeLoweringTimeMicros" : "<normalized>",
			"samplingTimeMicros" : "<normalized>",
			"cbrPlanningTimeMicros" : "<normalized>",
			"planEnumerationTimeMicros" : "<normalized>",
			"ceTimeMicros" : "<normalized>"
		},
		"collectionsWithoutPageCount" : [
			"shop.customers",
			"shop.orders",
			"shop.products",
			"shop.suppliers"
		]
	},
	"ceSamplingMetadata" : {
		"shop.customers" : {
			"sampleSource" : "onTheFly",
			"sampleTechnique" : "strides",
			"sampleRequestedDocCount" : 1063,
			"sampleDocCount" : 100,
			"sampleMemorySizeBytes" : 8762,
			"sampleCreatedAt" : "<normalized>"
		},
		"shop.orders" : {
			"sampleSource" : "onTheFly",
			"sampleTechnique" : "strides",
			"sampleRequestedDocCount" : 1063,
			"sampleDocCount" : 1000,
			"sampleMemorySizeBytes" : 117158,
			"sampleCreatedAt" : "<normalized>"
		},
		"shop.products" : {
			"sampleSource" : "onTheFly",
			"sampleTechnique" : "strides",
			"sampleRequestedDocCount" : 1063,
			"sampleDocCount" : 50,
			"sampleMemorySizeBytes" : 4398,
			"sampleCreatedAt" : "<normalized>"
		},
		"shop.suppliers" : {
			"sampleSource" : "onTheFly",
			"sampleTechnique" : "strides",
			"sampleRequestedDocCount" : 1063,
			"sampleDocCount" : 6,
			"sampleMemorySizeBytes" : 488,
			"sampleCreatedAt" : "<normalized>"
		}
	},
	"maxIndexedOrSolutionsReached" : false,
	"maxIndexedAndSolutionsReached" : false,
	"maxScansToExplodeReached" : false,
	"prunedSimilarIndexes" : false,
	"winningPlan" : {
		"isCached" : false,
		"usedJoinOptimization" : true,
		"queryPlan" : {
			"stage" : "GROUP",
			"planNodeId" : 11,
			"inputStage" : {
				"stage" : "NESTED_LOOP_JOIN_EMBEDDING",
				"planNodeId" : 10,
				"leftEmbeddingField" : "none",
				"rightEmbeddingField" : "supp",
				"joinPredicates" : [
					"prod.supplierCode = code"
				],
				"costEstimate" : "<normalized>",
				"cardinalityEstimate" : "<normalized>",
				"estimatesMetadata" : {
					"ceSource" : "Sampling"
				},
				"joinSelectivityEstimates" : [
					{
						"ndvs" : [
							{
								"nss" : "shop.suppliers",
								"ndv" : "<normalized>",
								"ndvSource" : "Sampling"
							}
						],
						"predicateSelectivity" : "<normalized>"
					}
				],
				"joinCostComponents" : {
					"docsProcessed" : "<normalized>",
					"docsOutput" : "<normalized>",
					"docsTransmitted" : "<normalized>",
					"sequentialIOs" : "<normalized>",
					"randomIOs" : "<normalized>",
					"localOpCost" : "<normalized>",
					"totalCost" : "<normalized>"
				},
				"inputStages" : [
					{
						"stage" : "HASH_JOIN_EMBEDDING",
						"planNodeId" : 8,
						"leftEmbeddingField" : "none",
						"rightEmbeddingField" : "prod",
						"joinPredicates" : [
							"productId = _id"
						],
						"costEstimate" : "<normalized>",
						"cardinalityEstimate" : "<normalized>",
						"estimatesMetadata" : {
							"ceSource" : "Metadata"
						},
						"joinSelectivityEstimates" : [
							{
								"ndvs" : [
									{
										"nss" : "shop.products",
										"ndv" : "<normalized>",
										"ndvSource" : "Metadata"
									}
								],
								"predicateSelectivity" : "<normalized>"
							}
						],
						"joinCostComponents" : {
							"docsProcessed" : "<normalized>",
							"docsOutput" : "<normalized>",
							"docsTransmitted" : "<normalized>",
							"sequentialIOs" : "<normalized>",
							"randomIOs" : "<normalized>",
							"localOpCost" : "<normalized>",
							"totalCost" : "<normalized>"
						},
						"inputStages" : [
							{
								"stage" : "INDEXED_NESTED_LOOP_JOIN_EMBEDDING",
								"planNodeId" : 6,
								"leftEmbeddingField" : "cust",
								"rightEmbeddingField" : "none",
								"joinPredicates" : [
									"_id = customerId"
								],
								"probePredicates" : [
									"_id = customerId"
								],
								"costEstimate" : "<normalized>",
								"cardinalityEstimate" : "<normalized>",
								"estimatesMetadata" : {
									"ceSource" : "Metadata"
								},
								"innerCardinalityBeforeJoinEstimate" : "<normalized>",
								"joinSelectivityEstimates" : [
									{
										"ndvs" : [
											{
												"nss" : "shop.customers",
												"ndv" : "<normalized>",
												"ndvSource" : "Metadata"
											}
										],
										"predicateSelectivity" : "<normalized>"
									}
								],
								"joinCostComponents" : {
									"docsProcessed" : "<normalized>",
									"docsOutput" : "<normalized>",
									"docsTransmitted" : "<normalized>",
									"sequentialIOs" : "<normalized>",
									"randomIOs" : "<normalized>",
									"localOpCost" : "<normalized>",
									"totalCost" : "<normalized>",
									"mackertLohmanCase" : "collection-fits-cache"
								},
								"inputStages" : [
									{
										"stage" : "FETCH",
										"planNodeId" : 2,
										"nss" : "shop.customers",
										"filter" : {
											"active" : {
												"$eq" : true
											}
										},
										"costEstimate" : "<normalized>",
										"cardinalityEstimate" : "<normalized>",
										"estimatesMetadata" : {
											"ceSource" : "Metadata"
										},
										"joinCostComponents" : {
											"docsProcessed" : "<normalized>",
											"docsOutput" : "<normalized>",
											"docsTransmitted" : "<normalized>",
											"sequentialIOs" : "<normalized>",
											"randomIOs" : "<normalized>",
											"localOpCost" : "<normalized>",
											"totalCost" : "<normalized>"
										},
										"inputStage" : {
											"stage" : "IXSCAN",
											"planNodeId" : 1,
											"nss" : "shop.customers",
											"keyPattern" : {
												"tier" : 1
											},
											"indexName" : "tier_1",
											"isMultiKey" : false,
											"multiKeyPaths" : {
												"tier" : [ ]
											},
											"isUnique" : false,
											"isSparse" : false,
											"isPartial" : false,
											"indexVersion" : 2,
											"direction" : "forward",
											"indexBounds" : {
												"tier" : [
													"[\"gold\", \"gold\"]"
												]
											}
										}
									},
									{
										"stage" : "PROJECTION_DEFAULT",
										"planNodeId" : 5,
										"transformBy" : {
											"customerId" : true,
											"productId" : true,
											"total" : true,
											"_id" : false
										},
										"inputStage" : {
											"stage" : "FETCH",
											"planNodeId" : 4,
											"nss" : "shop.orders",
											"filter" : {
												"$and" : [
													{
														"status" : {
															"$eq" : "shipped"
														}
													},
													{
														"total" : {
															"$gte" : 100
														}
													}
												]
											},
											"inputStage" : {
												"stage" : "INDEX_PROBE_NODE",
												"planNodeId" : 3,
												"nss" : "shop.orders",
												"keyPattern" : {
													"customerId" : 1
												},
												"indexName" : "customerId_1",
												"isMultiKey" : false,
												"isUnique" : false,
												"isSparse" : false,
												"isPartial" : false,
												"indexVersion" : 2
											}
										}
									}
								]
							},
							{
								"stage" : "COLLSCAN",
								"planNodeId" : 7,
								"nss" : "shop.products",
								"direction" : "forward",
								"filter" : {
									"category" : {
										"$in" : [
											"books",
											"toys"
										]
									}
								},
								"costEstimate" : "<normalized>",
								"cardinalityEstimate" : "<normalized>",
								"estimatesMetadata" : {
									"ceSource" : "Metadata"
								},
								"joinCostComponents" : {
									"docsProcessed" : "<normalized>",
									"docsOutput" : "<normalized>",
									"docsTransmitted" : "<normalized>",
									"sequentialIOs" : "<normalized>",
									"randomIOs" : "<normalized>",
									"localOpCost" : "<normalized>",
									"totalCost" : "<normalized>"
								}
							}
						]
					},
					{
						"stage" : "COLLSCAN",
						"planNodeId" : 9,
						"nss" : "shop.suppliers",
						"direction" : "forward",
						"filter" : {
							"region" : {
								"$eq" : "EMEA"
							}
						},
						"costEstimate" : "<normalized>",
						"cardinalityEstimate" : "<normalized>",
						"estimatesMetadata" : {
							"ceSource" : "Metadata"
						},
						"joinCostComponents" : {
							"docsProcessed" : "<normalized>",
							"docsOutput" : "<normalized>",
							"docsTransmitted" : "<normalized>",
							"sequentialIOs" : "<normalized>",
							"randomIOs" : "<normalized>",
							"localOpCost" : "<normalized>",
							"totalCost" : "<normalized>"
						}
					}
				]
			}
		},
		"slotBasedPlan" : "<normalized>"
	},
	"rejectedPlans" : [
		{
			"isCached" : false,
			"usedJoinOptimization" : true,
			"queryPlan" : {
				"stage" : "NESTED_LOOP_JOIN_EMBEDDING",
				"planNodeId" : 9,
				"leftEmbeddingField" : "none",
				"rightEmbeddingField" : "supp",
				"joinPredicates" : [
					"prod.supplierCode = code"
				],
				"costEstimate" : "<normalized>",
				"cardinalityEstimate" : "<normalized>",
				"estimatesMetadata" : {
					"ceSource" : "Sampling"
				},
				"joinSelectivityEstimates" : [
					{
						"ndvs" : [
							{
								"nss" : "shop.suppliers",
								"ndv" : "<normalized>",
								"ndvSource" : "Sampling"
							}
						],
						"predicateSelectivity" : "<normalized>"
					}
				],
				"joinCostComponents" : {
					"docsProcessed" : "<normalized>",
					"docsOutput" : "<normalized>",
					"docsTransmitted" : "<normalized>",
					"sequentialIOs" : "<normalized>",
					"randomIOs" : "<normalized>",
					"localOpCost" : "<normalized>",
					"totalCost" : "<normalized>"
				},
				"inputStages" : [
					{
						"stage" : "HASH_JOIN_EMBEDDING",
						"planNodeId" : 7,
						"leftEmbeddingField" : "none",
						"rightEmbeddingField" : "prod",
						"joinPredicates" : [
							"productId = _id"
						],
						"costEstimate" : "<normalized>",
						"cardinalityEstimate" : "<normalized>",
						"estimatesMetadata" : {
							"ceSource" : "Metadata"
						},
						"joinSelectivityEstimates" : [
							{
								"ndvs" : [
									{
										"nss" : "shop.products",
										"ndv" : "<normalized>",
										"ndvSource" : "Metadata"
									}
								],
								"predicateSelectivity" : "<normalized>"
							}
						],
						"joinCostComponents" : {
							"docsProcessed" : "<normalized>",
							"docsOutput" : "<normalized>",
							"docsTransmitted" : "<normalized>",
							"sequentialIOs" : "<normalized>",
							"randomIOs" : "<normalized>",
							"localOpCost" : "<normalized>",
							"totalCost" : "<normalized>"
						},
						"inputStages" : [
							{
								"stage" : "INDEXED_NESTED_LOOP_JOIN_EMBEDDING",
								"planNodeId" : 5,
								"leftEmbeddingField" : "none",
								"rightEmbeddingField" : "cust",
								"joinPredicates" : [
									"customerId = _id"
								],
								"probePredicates" : [
									"customerId = _id"
								],
								"costEstimate" : "<normalized>",
								"cardinalityEstimate" : "<normalized>",
								"estimatesMetadata" : {
									"ceSource" : "Metadata"
								},
								"innerCardinalityBeforeJoinEstimate" : "<normalized>",
								"joinSelectivityEstimates" : [
									{
										"ndvs" : [
											{
												"nss" : "shop.customers",
												"ndv" : "<normalized>",
												"ndvSource" : "Metadata"
											}
										],
										"predicateSelectivity" : "<normalized>"
									}
								],
								"joinCostComponents" : {
									"docsProcessed" : "<normalized>",
									"docsOutput" : "<normalized>",
									"docsTransmitted" : "<normalized>",
									"sequentialIOs" : "<normalized>",
									"randomIOs" : "<normalized>",
									"localOpCost" : "<normalized>",
									"totalCost" : "<normalized>",
									"mackertLohmanCase" : "collection-fits-cache"
								},
								"inputStages" : [
									{
										"stage" : "PROJECTION_SIMPLE",
										"planNodeId" : 2,
										"transformBy" : {
											"customerId" : true,
											"productId" : true,
											"total" : true,
											"_id" : false
										},
										"costEstimate" : "<normalized>",
										"cardinalityEstimate" : "<normalized>",
										"estimatesMetadata" : {
											"ceSource" : "Metadata"
										},
										"joinCostComponents" : {
											"docsProcessed" : "<normalized>",
											"docsOutput" : "<normalized>",
											"docsTransmitted" : "<normalized>",
											"sequentialIOs" : "<normalized>",
											"randomIOs" : "<normalized>",
											"localOpCost" : "<normalized>",
											"totalCost" : "<normalized>"
										},
										"inputStage" : {
											"stage" : "COLLSCAN",
											"planNodeId" : 1,
											"nss" : "shop.orders",
											"direction" : "forward",
											"filter" : {
												"$and" : [
													{
														"status" : {
															"$eq" : "shipped"
														}
													},
													{
														"total" : {
															"$gte" : 100
														}
													}
												]
											}
										}
									},
									{
										"stage" : "FETCH",
										"planNodeId" : 4,
										"nss" : "shop.customers",
										"filter" : {
											"$and" : [
												{
													"active" : {
														"$eq" : true
													}
												},
												{
													"tier" : {
														"$eq" : "gold"
													}
												}
											]
										},
										"inputStage" : {
											"stage" : "INDEX_PROBE_NODE",
											"planNodeId" : 3,
											"nss" : "shop.customers",
											"keyPattern" : {
												"_id" : 1
											},
											"indexName" : "_id_",
											"isMultiKey" : false,
											"isUnique" : true,
											"isSparse" : false,
											"isPartial" : false,
											"indexVersion" : 2
										}
									}
								]
							},
							{
								"stage" : "COLLSCAN",
								"planNodeId" : 6,
								"nss" : "shop.products",
								"direction" : "forward",
								"filter" : {
									"category" : {
										"$in" : [
											"books",
											"toys"
										]
									}
								},
								"costEstimate" : "<normalized>",
								"cardinalityEstimate" : "<normalized>",
								"estimatesMetadata" : {
									"ceSource" : "Metadata"
								},
								"joinCostComponents" : {
									"docsProcessed" : "<normalized>",
									"docsOutput" : "<normalized>",
									"docsTransmitted" : "<normalized>",
									"sequentialIOs" : "<normalized>",
									"randomIOs" : "<normalized>",
									"localOpCost" : "<normalized>",
									"totalCost" : "<normalized>"
								}
							}
						]
					},
					{
						"stage" : "COLLSCAN",
						"planNodeId" : 8,
						"nss" : "shop.suppliers",
						"direction" : "forward",
						"filter" : {
							"region" : {
								"$eq" : "EMEA"
							}
						},
						"costEstimate" : "<normalized>",
						"cardinalityEstimate" : "<normalized>",
						"estimatesMetadata" : {
							"ceSource" : "Metadata"
						},
						"joinCostComponents" : {
							"docsProcessed" : "<normalized>",
							"docsOutput" : "<normalized>",
							"docsTransmitted" : "<normalized>",
							"sequentialIOs" : "<normalized>",
							"randomIOs" : "<normalized>",
							"localOpCost" : "<normalized>",
							"totalCost" : "<normalized>"
						}
					}
				]
			},
			"slotBasedPlan" : "<normalized>"
		}
	]
}
```
