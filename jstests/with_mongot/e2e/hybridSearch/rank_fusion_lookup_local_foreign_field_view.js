/**
 * Tests $rankFusion inside a $lookup sub-pipeline that uses localField/foreignField join syntax,
 * when the $lookup targets a view. Verifies that the view filter is applied to hybrid search
 * results, including when $rankFusion has multiple input pipelines (which desugar to $unionWith).
 *
 * @tags: [
 *   featureFlagSearchHybridScoringFull,
 *   featureFlagExtensionsInsideHybridSearch,
 *   requires_fcv_82,
 * ]
 */
import {createSearchIndex} from "jstests/libs/query_integration_search/search.js";
import {before, describe, it} from "jstests/libs/mochalite.js";

const productsName = jsTestName() + "_products";
const ordersName = jsTestName() + "_orders";
const viewName = jsTestName() + "_instock";
const searchIndexName = "idx";

const products = db.getCollection(productsName);
const orders = db.getCollection(ordersName);

describe("$rankFusion in $lookup with localField/foreignField on a view", function () {
    before(function () {
        products.drop();
        orders.drop();
        // Do not access db[viewName] before the view is created. In the sharded_collections
        // passthrough, implicitly_shard_accessed_collections.js shards any accessed empty
        // namespace, which would create the view's namespace as a collection and make the
        // createView below fail with NamespaceExists. The fixture is fresh per test, so no
        // pre-drop of the view is needed.

        assert.commandWorked(
            products.insertMany([
                {_id: 1, name: "Laptop", description: "powerful computing device", inStock: true},
                {
                    _id: 2,
                    name: "Phone",
                    description: "portable communication device",
                    inStock: true,
                },
                {_id: 3, name: "Desk", description: "wooden office desk", inStock: false},
                {_id: 4, name: "Chair", description: "comfortable office chair", inStock: true},
                {_id: 5, name: "Tablet", description: "portable computing tablet", inStock: false},
            ]),
        );

        assert.commandWorked(
            orders.insertMany([
                {_id: 101, customer: "Alice", product_id: 1}, // in-stock product
                {_id: 102, customer: "Bob", product_id: 3}, // out-of-stock product
                {_id: 103, customer: "Carol", product_id: 4}, // in-stock product
                {_id: 104, customer: "Dave", product_id: 5}, // out-of-stock product
            ]),
        );

        // View filters to only in-stock products. Mongot requires $match stages in view definitions
        // to use $expr.
        assert.commandWorked(
            db.createView(viewName, productsName, [{$match: {$expr: {$eq: ["$inStock", true]}}}]),
        );

        // For mongot-indexed views, the search index must be created on the VIEW namespace so that
        // mongot can find it when $search carries a view field.
        createSearchIndex(db[viewName], {
            name: searchIndexName,
            definition: {mappings: {dynamic: true}},
        });
    });

    it("single-pipeline $rankFusion: view filter applied, out-of-stock products excluded", function () {
        // Query "office" matches product 3 (Desk, out-of-stock) and product 4 (Chair, in-stock),
        // so the view filter is what excludes Bob's result and includes Carol's.
        const results = orders
            .aggregate([
                {
                    $lookup: {
                        from: viewName,
                        localField: "product_id",
                        foreignField: "_id",
                        pipeline: [
                            {
                                $rankFusion: {
                                    input: {
                                        pipelines: {
                                            a: [
                                                {
                                                    $search: {
                                                        index: searchIndexName,
                                                        text: {
                                                            query: "office",
                                                            path: "description",
                                                        },
                                                    },
                                                },
                                            ],
                                        },
                                    },
                                },
                            },
                        ],
                        as: "product",
                    },
                },
            ])
            .toArray();

        const byCustomer = Object.fromEntries(results.map((r) => [r.customer, r.product]));
        // Carol's product (Chair, "comfortable office chair") is in-stock and matches "office".
        assert.eq(byCustomer["Carol"].length, 1, "Carol should match in-stock product 4", {
            byCustomer,
        });
        assert.eq(byCustomer["Carol"][0]._id, 4);
        // Alice's product (Laptop, "powerful computing device") does not match "office".
        assert.eq(
            byCustomer["Alice"].length,
            0,
            "Alice's product does not match the search query",
            {byCustomer},
        );
        // Bob's product (Desk, "wooden office desk") matches "office" but is out-of-stock:
        // the view filter must exclude it.
        assert.eq(
            byCustomer["Bob"].length,
            0,
            "Bob's product is out of stock, view should exclude it",
            {byCustomer},
        );
        assert.eq(
            byCustomer["Dave"].length,
            0,
            "Dave's product is out of stock, view should exclude it",
            {byCustomer},
        );
    });

    it("multi-pipeline $rankFusion: view filter applied across desugared $unionWith stages", function () {
        const results = orders
            .aggregate([
                {
                    $lookup: {
                        from: viewName,
                        localField: "product_id",
                        foreignField: "_id",
                        pipeline: [
                            {
                                $rankFusion: {
                                    input: {
                                        pipelines: {
                                            a: [
                                                {
                                                    $search: {
                                                        index: searchIndexName,
                                                        text: {
                                                            query: "device",
                                                            path: "description",
                                                        },
                                                    },
                                                },
                                            ],
                                            b: [
                                                {
                                                    $search: {
                                                        index: searchIndexName,
                                                        text: {
                                                            query: "office",
                                                            path: "description",
                                                        },
                                                    },
                                                },
                                            ],
                                        },
                                    },
                                },
                            },
                        ],
                        as: "product",
                    },
                },
            ])
            .toArray();

        const byCustomer = Object.fromEntries(results.map((r) => [r.customer, r.product]));
        assert.eq(byCustomer["Alice"].length, 1, "Alice should match in-stock product 1", {
            byCustomer,
        });
        assert.eq(byCustomer["Alice"][0]._id, 1);
        assert.eq(byCustomer["Carol"].length, 1, "Carol should match in-stock product 4", {
            byCustomer,
        });
        assert.eq(byCustomer["Carol"][0]._id, 4);
        // View must exclude out-of-stock products from the $unionWith-desugared pipelines too.
        assert.eq(byCustomer["Bob"].length, 0, "view should exclude out-of-stock product 3", {
            byCustomer,
        });
        assert.eq(byCustomer["Dave"].length, 0, "view should exclude out-of-stock product 5", {
            byCustomer,
        });
    });

    it("multi-pipeline $rankFusion with trailing stages: join filters before trailing $set", function () {
        // A $set AFTER $rankFusion mutates the join field (_id). The localField/foreignField
        // equality must be evaluated against the ORIGINAL _id (right after the hybrid search
        // desugaring, before the $set), not the mutated value. If the join $match were positioned
        // after the trailing stages, the comparison product_id == (_id + 100) would never match
        // and every result would be empty.
        const results = orders
            .aggregate([
                {
                    $lookup: {
                        from: viewName,
                        localField: "product_id",
                        foreignField: "_id",
                        pipeline: [
                            {
                                $rankFusion: {
                                    input: {
                                        pipelines: {
                                            a: [
                                                {
                                                    $search: {
                                                        index: searchIndexName,
                                                        text: {
                                                            query: "device",
                                                            path: "description",
                                                        },
                                                    },
                                                },
                                            ],
                                            b: [
                                                {
                                                    $search: {
                                                        index: searchIndexName,
                                                        text: {
                                                            query: "office",
                                                            path: "description",
                                                        },
                                                    },
                                                },
                                            ],
                                        },
                                    },
                                },
                            },
                            {$set: {_id: {$add: ["$_id", 100]}}},
                        ],
                        as: "product",
                    },
                },
            ])
            .toArray();

        const byCustomer = Object.fromEntries(results.map((r) => [r.customer, r.product]));
        // Alice's product (original _id 1) matches "device" and is in-stock; after the trailing
        // $set its _id is 101. The join must still have matched on the original _id.
        assert.eq(
            byCustomer["Alice"].length,
            1,
            "Alice should match in-stock product 1 (join on original _id)",
            {
                byCustomer,
            },
        );
        assert.eq(byCustomer["Alice"][0]._id, 101);
        // Carol's product (original _id 4) matches "office" and is in-stock; _id becomes 104.
        assert.eq(byCustomer["Carol"].length, 1, "Carol should match in-stock product 4", {
            byCustomer,
        });
        assert.eq(byCustomer["Carol"][0]._id, 104);
        // Out-of-stock products are still excluded by the view.
        assert.eq(byCustomer["Bob"].length, 0, "view should exclude out-of-stock product 3", {
            byCustomer,
        });
        assert.eq(byCustomer["Dave"].length, 0, "view should exclude out-of-stock product 5", {
            byCustomer,
        });
    });
});
