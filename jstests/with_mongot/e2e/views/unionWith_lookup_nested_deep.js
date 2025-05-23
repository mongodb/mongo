/**
 * This test nests multiple $lookup and $unionWith stages within each other in two different
 * manners: one starting with a $unionWith and the other starting with a $lookup. In either case,
 * all stages have a $search query on a view. The test verifies that the results are as expected
 * with the respective view definition applied to each document.
 *
 * @tags: [ requires_fcv_81, featureFlagMongotIndexedViews ]
 */
import {assertArrayEq} from "jstests/aggregation/extras/utils.js";
import {
    assertUnionWithSearchSubPipelineAppliedViews,
} from "jstests/with_mongot/e2e_lib/explain_utils.js";
import {
    createSearchIndexesAndExecuteTests,
    validateSearchExplain
} from "jstests/with_mongot/e2e_lib/search_e2e_utils.js";

const testDb = db.getSiblingDB(jsTestName());
const products = testDb.products;
const categories = testDb.categories;
const suppliers = testDb.suppliers;
const customers = testDb.customers;
const orders = testDb.orders;
products.drop();
categories.drop();
suppliers.drop();
customers.drop();
orders.drop();

assert.commandWorked(products.insertMany([
    {
        _id: 1,
        name: "Premium Coffee Beans",
        price: 24.99,
        category_id: 51,
        supplier_id: 101,
        tags: ["organic", "premium", "fair-trade"],
        origin: "Colombia"
    },
    {
        _id: 2,
        name: "Espresso Machine Pro",
        price: 349.99,
        category_id: 52,
        supplier_id: 102,
        tags: ["premium", "appliance", "stainless-steel"],
        origin: "Italy"
    },
    {
        _id: 3,
        name: "Coffee Grinder",
        price: 79.99,
        category_id: 52,
        supplier_id: 102,
        tags: ["appliance", "essential"],
        origin: "Germany"
    },
    {
        _id: 4,
        name: "Pour Over Coffee Set",
        price: 45.99,
        category_id: 53,
        supplier_id: 103,
        tags: ["manual", "glass", "premium"],
        origin: "Japan"
    },
    {
        _id: 5,
        name: "Specialty Tea Sampler",
        price: 29.99,
        category_id: 54,
        supplier_id: 104,
        tags: ["organic", "variety", "gift"],
        origin: "China"
    }
]));

assert.commandWorked(categories.insertMany([
    {
        _id: 51,
        name: "Coffee Beans",
        description: "Freshly roasted coffee beans from around the world",
        parent_id: null,
        popularity_score: 95
    },
    {
        _id: 52,
        name: "Brewing Equipment",
        description: "Professional and home coffee brewing machines and equipment",
        parent_id: null,
        popularity_score: 88
    },
    {
        _id: 53,
        name: "Manual Brewers",
        description: "Hand brewing equipment for coffee enthusiasts",
        parent_id: 2,
        popularity_score: 75
    },
    {
        _id: 54,
        name: "Tea Collections",
        description: "Premium tea collections from global sources",
        parent_id: null,
        popularity_score: 82
    }
]));

assert.commandWorked(suppliers.insertMany([
    {
        _id: 101,
        name: "Global Bean Traders",
        location: "Portland, USA",
        product_categories: [51],
        rating: 4.9,
    },
    {
        _id: 102,
        name: "EuroKitchen Supplies",
        location: "Milan, Italy",
        product_categories: [52, 53],
        rating: 4.7,
    },
    {
        _id: 103,
        name: "Artisan Brew Co.",
        location: "Tokyo, Japan",
        product_categories: [52, 53],
        rating: 4.8,
    },
    {
        _id: 104,
        name: "Tea Leaf Imports",
        location: "Shanghai, China",
        product_categories: [54],
        rating: 4.6,
    }
]));

assert.commandWorked(customers.insertMany([
    {
        _id: 201,
        name: "Coffee House Downtown",
        type: "business",
        location: "New York, USA",
        tier: "premium",
        favorite_categories: [1, 2]
    },
    {
        _id: 202,
        name: "Sarah Johnson",
        type: "individual",
        location: "Seattle, USA",
        tier: "regular",
        favorite_categories: [1, 3]
    },
    {
        _id: 203,
        name: "Specialty Café Chain",
        type: "business",
        location: "Chicago, USA",
        tier: "premium",
        favorite_categories: [1, 2, 4]
    },
    {
        _id: 204,
        name: "Tea & Coffee Emporium",
        type: "business",
        location: "San Francisco, USA",
        tier: "premium",
        favorite_categories: [1, 4]
    }
]));

assert.commandWorked(orders.insertMany([
    {
        _id: 1001,
        customer_id: 201,
        date: new Date("2023-01-15"),
        products: [
            {product_id: 1, quantity: 50, price: 22.99},
            {product_id: 2, quantity: 2, price: 339.99}
        ],
        total: 1829.98,
        status: "delivered",
    },
    {
        _id: 1002,
        customer_id: 202,
        date: new Date("2023-01-20"),
        products: [
            {product_id: 1, quantity: 2, price: 24.99},
            {product_id: 4, quantity: 1, price: 45.99}
        ],
        total: 95.97,
        status: "delivered",
    },
    {
        _id: 1003,
        customer_id: 203,
        date: new Date("2023-02-01"),
        products: [
            {product_id: 1, quantity: 100, price: 21.99},
            {product_id: 3, quantity: 10, price: 75.99}
        ],
        total: 2959.90,
        status: "processing",
    },
    {
        _id: 1004,
        customer_id: 204,
        date: new Date("2023-02-05"),
        products: [
            {product_id: 5, quantity: 40, price: 28.99},
            {product_id: 1, quantity: 30, price: 23.99}
        ],
        total: 1879.30,
        status: "shipped",
    }
]));

// Create views on all collections.
const productsViewPipeline = [{
    $addFields: {
        display_name: {$concat: ["$name", " - $", {$toString: {$round: ["$price", 2]}}]},
    }
}];
assert.commandWorked(testDb.createView("productsView", products.getName(), productsViewPipeline));
const productsView = testDb.productsView;

const categoriesViewPipeline = [{
    $addFields: {
        display_name: {$concat: ["$name", " (", {$toString: "$popularity_score"}, "% popularity)"]},
        popularity_tier: {
            $switch: {
                branches: [
                    {case: {$gte: ["$popularity_score", 90]}, then: "top-tier"},
                    {case: {$gte: ["$popularity_score", 80]}, then: "high-tier"},
                    {case: {$gte: ["$popularity_score", 70]}, then: "medium-tier"}
                ],
                default: "standard-tier"
            }
        }
    }
}];
assert.commandWorked(
    testDb.createView("categoriesView", categories.getName(), categoriesViewPipeline));
const categoriesView = testDb.categoriesView;

const suppliersViewPipeline = [{
    $addFields: {
        full_name: {$concat: ["$name", " (", "$location", ")"]},
        supplier_tier: {
            $switch: {
                branches: [
                    {case: {$gte: ["$rating", 4.8]}, then: "platinum-supplier"},
                    {case: {$gte: ["$rating", 4.5]}, then: "gold-supplier"}
                ],
                default: "standard-supplier"
            }
        }
    }
}];
assert.commandWorked(
    testDb.createView("suppliersView", suppliers.getName(), suppliersViewPipeline));
const suppliersView = testDb.suppliersView;

const customersViewPipeline = [{
    $addFields: {
        customer_profile: {$concat: ["$name", " (", "$type", ")"]},
        value_segment: {
            $switch: {
                branches: [
                    {case: {$eq: ["$tier", "premium"]}, then: "high-value"},
                    {case: {$eq: ["$tier", "regular"]}, then: "mid-value"}
                ],
                default: "standard-value"
            }
        }
    }
}];
assert.commandWorked(
    testDb.createView("customersView", customers.getName(), customersViewPipeline));
const customersView = testDb.customersView;

const ordersViewPipeline = [{
    $addFields: {
        order_summary: {
            $concat: ["Order #", {$toString: "$_id"}, " - $", {$toString: {$round: ["$total", 2]}}]
        },
        order_size: {
            $switch: {
                branches: [
                    {case: {$gte: ["$total", 1000]}, then: "large-order"},
                    {case: {$gte: ["$total", 500]}, then: "medium-order"},
                    {case: {$gte: ["$total", 100]}, then: "small-order"}
                ],
                default: "mini-order"
            }
        }
    }
}];
assert.commandWorked(testDb.createView("ordersView", orders.getName(), ordersViewPipeline));
const ordersView = testDb.ordersView;

// Create search indexes on all views.
const productsIndexName = "productsIndex";
const categoriesIndexName = "categoriesIndex";
const suppliersIndexName = "suppliersIndex";
const customersIndexName = "customersIndex";
const ordersIndexName = "ordersIndex";
const indexConfigs = [
    {
        coll: productsView,
        definition: {name: productsIndexName, definition: {mappings: {dynamic: true}}}
    },
    {
        coll: categoriesView,
        definition: {name: categoriesIndexName, definition: {mappings: {dynamic: true}}}
    },
    {
        coll: suppliersView,
        definition: {name: suppliersIndexName, definition: {mappings: {dynamic: true}}}
    },
    {
        coll: customersView,
        definition: {name: customersIndexName, definition: {mappings: {dynamic: true}}}
    },
    {coll: ordersView, definition: {name: ordersIndexName, definition: {mappings: {dynamic: true}}}}
];

const unionWithLookupNestedDeepTestCases = (isStoredSource) => {
    // ===============================================================================
    // Case 1: Deep nested pipeline that begins with $unionWith.
    // ===============================================================================

    // This pipeline starts with $unionWith and then alternates between different operations:
    // 1. Top level: Searches for products with "premium" in tags.
    // 2. First level: $unionWith to categories that have "coffee" in description.
    // 3. Second level: $lookup to find suppliers with product_categories matching the category _id.
    //    - Inside suppliers lookup: Search for suppliers in "Japan".
    // 4. Third level: $unionWith to customers with "premium" tier.
    // 5. Fourth level: $lookup to find orders from these customers.
    //    - Inside orders lookup: Search for orders with "delivered" status.
    //
    // The result is a combination of:
    // - Products that have "premium" in their tags.
    // - Categories related to "coffee" that have suppliers in Japan OR premium customers with
    // delivered orders.
    const unionWithFirstPipeline = [
            {$search: {index: productsIndexName, text: {query: "premium", path: "tags"}, returnStoredSource: isStoredSource}},
            {
                $unionWith: {
                    coll: categoriesView.getName(),
                    pipeline: [
                        {
                            $search: {
                                index: categoriesIndexName,
                                text: {query: "coffee", path: "description"},
                                returnStoredSource: isStoredSource
                            }
                        },
                        {
                            $lookup: {
                                from: suppliersView.getName(),
                                localField: "_id",
                                foreignField: "product_categories",
                                as: "suppliers",
                                pipeline: [
                                    {
                                        $search: {
                                            index: suppliersIndexName,
                                            text: {query: "Japan", path: "location"},
                                            returnStoredSource: isStoredSource
                                        }
                                    },
                                    {
                                        $unionWith: {
                                            coll: customersView.getName(),  
                                            pipeline: [
                                                {
                                                        $search: {
                                                        index: customersIndexName,
                                                        text: {query: "premium", path: "tier"},
                                                        returnStoredSource: isStoredSource 
                                                    }
                                                },
                                                {
                                                    $lookup: {
                                                        from: ordersView.getName(),
                                                        localField: "_id",
                                                        foreignField: "customer_id",
                                                        as: "orders",
                                                        pipeline: [
                                                            {
                                                                $search: {
                                                                    index: ordersIndexName,
                                                                    text: {
                                                                        query: "delivered",
                                                                        path: "status"
                                                                    },
                                                                    returnStoredSource: isStoredSource
                                                                }
                                                            },
                                                            {$sort: {_id: 1}}
                                                        ]
                                                    }
                                                },
                                                {
                                                    $match: {
                                                        orders: {$ne: []}
                                                    }
                                                },
                                                {$sort: {_id: 1}}
                                            ]
                                        }
                                    },
                                    {$sort: {_id: 1}}
                                ]
                            }
                        },
                        {
                            $match: {
                                suppliers: {$ne: []}
                            }
                        },
                        {$sort: {_id: 1}}
                    ]
                }
            },
            {$sort: {_id: 1}}
        ];

    const unionWithFirstExpected = [
        {
            _id: 1,
            name: "Premium Coffee Beans",
            price: 24.99,
            category_id: 51,
            supplier_id: 101,
            tags: ["organic", "premium", "fair-trade"],
            origin: "Colombia",
            display_name: "Premium Coffee Beans - $24.99",
        },
        {
            _id: 2,
            name: "Espresso Machine Pro",
            price: 349.99,
            category_id: 52,
            supplier_id: 102,
            tags: ["premium", "appliance", "stainless-steel"],
            origin: "Italy",
            display_name: "Espresso Machine Pro - $349.99",
        },
        {
            _id: 4,
            name: "Pour Over Coffee Set",
            price: 45.99,
            category_id: 53,
            supplier_id: 103,
            tags: ["manual", "glass", "premium"],
            origin: "Japan",
            display_name: "Pour Over Coffee Set - $45.99",
        },
        {
            _id: 51,
            name: "Coffee Beans",
            description: "Freshly roasted coffee beans from around the world",
            parent_id: null,
            popularity_score: 95,
            display_name: "Coffee Beans (95% popularity)",
            popularity_tier: "top-tier",
            suppliers: [{
                _id: 201,
                name: "Coffee House Downtown",
                type: "business",
                location: "New York, USA",
                tier: "premium",
                favorite_categories: [1, 2],
                customer_profile: "Coffee House Downtown (business)",
                value_segment: "high-value",
                orders: [{
                    _id: 1001,
                    customer_id: 201,
                    date: ISODate("2023-01-15T00:00:00Z"),
                    products: [
                        {product_id: 1, quantity: 50, price: 22.99},
                        {product_id: 2, quantity: 2, price: 339.99}
                    ],
                    total: 1829.98,
                    status: "delivered",
                    order_summary: "Order #1001 - $1829.98",
                    order_size: "large-order"
                }]
            }]
        },
        {
            _id: 52,
            name: "Brewing Equipment",
            description: "Professional and home coffee brewing machines and equipment",
            parent_id: null,
            popularity_score: 88,
            display_name: "Brewing Equipment (88% popularity)",
            popularity_tier: "high-tier",
            suppliers: [
                {
                    _id: 103,
                    name: "Artisan Brew Co.",
                    location: "Tokyo, Japan",
                    product_categories: [52, 53],
                    rating: 4.8,
                    full_name: "Artisan Brew Co. (Tokyo, Japan)",
                    supplier_tier: "platinum-supplier"
                },
                {
                    _id: 201,
                    name: "Coffee House Downtown",
                    type: "business",
                    location: "New York, USA",
                    tier: "premium",
                    favorite_categories: [1, 2],
                    customer_profile: "Coffee House Downtown (business)",
                    value_segment: "high-value",
                    orders: [{
                        _id: 1001,
                        customer_id: 201,
                        date: ISODate("2023-01-15T00:00:00Z"),
                        products: [
                            {product_id: 1, quantity: 50, price: 22.99},
                            {product_id: 2, quantity: 2, price: 339.99}
                        ],
                        total: 1829.98,
                        status: "delivered",
                        order_summary: "Order #1001 - $1829.98",
                        order_size: "large-order"
                    }]
                }
            ]
        },
        {
            _id: 53,
            name: "Manual Brewers",
            description: "Hand brewing equipment for coffee enthusiasts",
            parent_id: 2,
            popularity_score: 75,
            display_name: "Manual Brewers (75% popularity)",
            popularity_tier: "medium-tier",
            suppliers: [
                {
                    _id: 103,
                    name: "Artisan Brew Co.",
                    location: "Tokyo, Japan",
                    product_categories: [52, 53],
                    rating: 4.8,
                    full_name: "Artisan Brew Co. (Tokyo, Japan)",
                    supplier_tier: "platinum-supplier"
                },
                {
                    _id: 201,
                    name: "Coffee House Downtown",
                    type: "business",
                    location: "New York, USA",
                    tier: "premium",
                    favorite_categories: [1, 2],
                    customer_profile: "Coffee House Downtown (business)",
                    value_segment: "high-value",
                    orders: [{
                        _id: 1001,
                        customer_id: 201,
                        date: ISODate("2023-01-15T00:00:00Z"),
                        products: [
                            {product_id: 1, quantity: 50, price: 22.99},
                            {product_id: 2, quantity: 2, price: 339.99}
                        ],
                        total: 1829.98,
                        status: "delivered",
                        order_summary: "Order #1001 - $1829.98",
                        order_size: "large-order"
                    }]
                }
            ]
        }
    ];

    validateSearchExplain(productsView,
                          unionWithFirstPipeline,
                          isStoredSource,
                          productsViewPipeline,
                          (unionWithFirstExplain) => {
                              // Assert that the first level $unionWith view definition is applied
                              // in the explain output. We don't currently have the testing
                              // infrastructure to check for inner $unionWith view definitions.
                              assertUnionWithSearchSubPipelineAppliedViews(unionWithFirstExplain,
                                                                           categories,
                                                                           categoriesView,
                                                                           categoriesViewPipeline,
                                                                           isStoredSource);
                          });

    let results = productsView.aggregate(unionWithFirstPipeline).toArray();
    assertArrayEq({actual: results, expected: unionWithFirstExpected});

    // ===============================================================================
    // Case 2: Deep nested pipeline that begins with $lookup.
    // ===============================================================================

    // This pipeline starts with $lookup and then alternates between different operations:
    // 1. Start: Search for products with "organic" in tags.
    // 2. First level: $lookup to categories where product's category_id matches category's _id.
    //    - Inside categories lookup: Search for categories with "coffee" in name.
    // 3. Second level: $unionWith to suppliers.
    //    - Inside suppliers unionWith: Search for suppliers with "USA" in location.
    // 4. Third level: $lookup to find customers.
    //    - Inside customers lookup: Search for customers with "business" type.
    // 5. Fourth level: $unionWith to orders with "large" in order_size.
    //
    // The result includes:
    // - Organic products that either:
    //   - Have a category related to coffee, OR
    //   - Have suppliers in the USA who have business customers OR large orders.
    const lookupFirstPipeline = [
            {$search: {index: productsIndexName, text: {query: "organic", path: "tags"}, returnStoredSource: isStoredSource}},
            {
                $lookup: {
                    from: categoriesView.getName(),
                    localField: "category_id",
                    foreignField: "_id",
                    as: "category_info",
                    pipeline: [
                        {
                            $search: {
                                index: categoriesIndexName,
                                text: {
                                    query: "coffee",
                                    path: "name"
                                },
                                returnStoredSource: isStoredSource
                            }
                        },
                        {
                            $unionWith: {
                                coll: suppliersView.getName(),
                                pipeline: [
                                    {
                                        $search: {
                                            index: suppliersIndexName,
                                            text: {
                                                query: "USA",
                                                path: "location"
                                            },
                                            returnStoredSource: isStoredSource
                                        }
                                    },
                                    {
                                        $lookup: {
                                            from: customersView.getName(),  
                                            pipeline: [
                                                {
                                                    $search: {
                                                        index: customersIndexName,
                                                        text: {
                                                            query: "business",
                                                            path: "type"
                                                        },
                                                        returnStoredSource: isStoredSource
                                                    }
                                                },
                                                {
                                                    $unionWith: {
                                                        coll: ordersView.getName(),
                                                        pipeline: [
                                                            {
                                                                $search: {
                                                                    index: ordersIndexName,
                                                                    text: {
                                                                        query: "large",
                                                                        path: "order_size"
                                                                    },
                                                                    returnStoredSource: isStoredSource
                                                                }
                                                            },
                                                            {$sort: {_id: 1}}
                                                        ]
                                                    }
                                                },
                                                {$sort: {_id: 1}}
                                            ],
                                            as: "interested_customers"
                                        }
                                    },
                                    {$sort: {_id: 1}}
                                ]
                            }
                        },
                        {$sort: {_id: 1}}
                    ]
                }
            },
            {
                $match: {
                    category_info: {$ne: []}
                }
            },
            {$sort: {_id: 1}}
        ];

    const lookupFirstExpected = [
        {
            _id: 1,
            name: "Premium Coffee Beans",
            price: 24.99,
            category_id: 51,
            supplier_id: 101,
            tags: ["organic", "premium", "fair-trade"],
            origin: "Colombia",
            display_name: "Premium Coffee Beans - $24.99",
            category_info: [
                {
                    _id: 51,
                    name: "Coffee Beans",
                    description: "Freshly roasted coffee beans from around the world",
                    parent_id: null,
                    popularity_score: 95,
                    display_name: "Coffee Beans (95% popularity)",
                    popularity_tier: "top-tier"
                },
                {
                    _id: 101,
                    name: "Global Bean Traders",
                    location: "Portland, USA",
                    product_categories: [51],
                    rating: 4.9,
                    full_name: "Global Bean Traders (Portland, USA)",
                    supplier_tier: "platinum-supplier",
                    interested_customers: [
                        {
                            _id: 201,
                            name: "Coffee House Downtown",
                            type: "business",
                            location: "New York, USA",
                            tier: "premium",
                            favorite_categories: [1, 2],
                            customer_profile: "Coffee House Downtown (business)",
                            value_segment: "high-value"
                        },
                        {
                            _id: 203,
                            name: "Specialty Café Chain",
                            type: "business",
                            location: "Chicago, USA",
                            tier: "premium",
                            favorite_categories: [1, 2, 4],
                            customer_profile: "Specialty Café Chain (business)",
                            value_segment: "high-value"
                        },
                        {
                            _id: 204,
                            name: "Tea & Coffee Emporium",
                            type: "business",
                            location: "San Francisco, USA",
                            tier: "premium",
                            favorite_categories: [1, 4],
                            customer_profile: "Tea & Coffee Emporium (business)",
                            value_segment: "high-value"
                        },
                        {
                            _id: 1001,
                            customer_id: 201,
                            date: ISODate("2023-01-15T00:00:00Z"),
                            products: [
                                {product_id: 1, quantity: 50, price: 22.99},
                                {product_id: 2, quantity: 2, price: 339.99}
                            ],
                            total: 1829.98,
                            status: "delivered",
                            order_summary: "Order #1001 - $1829.98",
                            order_size: "large-order"
                        },
                        {
                            _id: 1003,
                            customer_id: 203,
                            date: ISODate("2023-02-01T00:00:00Z"),
                            products: [
                                {product_id: 1, quantity: 100, price: 21.99},
                                {product_id: 3, quantity: 10, price: 75.99}
                            ],
                            total: 2959.9,
                            status: "processing",
                            order_summary: "Order #1003 - $2959.9",
                            order_size: "large-order"
                        },
                        {
                            _id: 1004,
                            customer_id: 204,
                            date: ISODate("2023-02-05T00:00:00Z"),
                            products: [
                                {product_id: 5, quantity: 40, price: 28.99},
                                {product_id: 1, quantity: 30, price: 23.99}
                            ],
                            total: 1879.3,
                            status: "shipped",
                            order_summary: "Order #1004 - $1879.3",
                            order_size: "large-order"
                        }
                    ]
                }
            ]
        },
        {
            _id: 5,
            name: "Specialty Tea Sampler",
            price: 29.99,
            category_id: 54,
            supplier_id: 104,
            tags: ["organic", "variety", "gift"],
            origin: "China",
            display_name: "Specialty Tea Sampler - $29.99",
            category_info: [{
                _id: 101,
                name: "Global Bean Traders",
                location: "Portland, USA",
                product_categories: [51],
                rating: 4.9,
                full_name: "Global Bean Traders (Portland, USA)",
                supplier_tier: "platinum-supplier",
                interested_customers: [
                    {
                        _id: 201,
                        name: "Coffee House Downtown",
                        type: "business",
                        location: "New York, USA",
                        tier: "premium",
                        favorite_categories: [1, 2],
                        customer_profile: "Coffee House Downtown (business)",
                        value_segment: "high-value"
                    },
                    {
                        _id: 203,
                        name: "Specialty Café Chain",
                        type: "business",
                        location: "Chicago, USA",
                        tier: "premium",
                        favorite_categories: [1, 2, 4],
                        customer_profile: "Specialty Café Chain (business)",
                        value_segment: "high-value"
                    },
                    {
                        _id: 204,
                        name: "Tea & Coffee Emporium",
                        type: "business",
                        location: "San Francisco, USA",
                        tier: "premium",
                        favorite_categories: [1, 4],
                        customer_profile: "Tea & Coffee Emporium (business)",
                        value_segment: "high-value"
                    },
                    {
                        _id: 1001,
                        customer_id: 201,
                        date: ISODate("2023-01-15T00:00:00Z"),
                        products: [
                            {product_id: 1, quantity: 50, price: 22.99},
                            {product_id: 2, quantity: 2, price: 339.99}
                        ],
                        total: 1829.98,
                        status: "delivered",
                        order_summary: "Order #1001 - $1829.98",
                        order_size: "large-order"
                    },
                    {
                        _id: 1003,
                        customer_id: 203,
                        date: ISODate("2023-02-01T00:00:00Z"),
                        products: [
                            {product_id: 1, quantity: 100, price: 21.99},
                            {product_id: 3, quantity: 10, price: 75.99}
                        ],
                        total: 2959.9,
                        status: "processing",
                        order_summary: "Order #1003 - $2959.9",
                        order_size: "large-order"
                    },
                    {
                        _id: 1004,
                        customer_id: 204,
                        date: ISODate("2023-02-05T00:00:00Z"),
                        products: [
                            {product_id: 5, quantity: 40, price: 28.99},
                            {product_id: 1, quantity: 30, price: 23.99}
                        ],
                        total: 1879.3,
                        status: "shipped",
                        order_summary: "Order #1004 - $1879.3",
                        order_size: "large-order"
                    }
                ]
            }]
        }
    ];

    validateSearchExplain(productsView, lookupFirstPipeline, isStoredSource, productsViewPipeline);

    results = productsView.aggregate(lookupFirstPipeline).toArray();
    assertArrayEq({actual: results, expected: lookupFirstExpected});
};

createSearchIndexesAndExecuteTests(indexConfigs, unionWithLookupNestedDeepTestCases);
