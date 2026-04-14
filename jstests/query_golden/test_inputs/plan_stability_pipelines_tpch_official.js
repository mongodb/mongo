/**
 * This file contains the official MQL representations of the queries from the TPC-H benchmark.
 */

export const commands = [
    {
        "idx": "1",
        "aggregate": "lineitem",
        "pipeline": [
            {
                "$match": {
                    "$expr": {
                        "$lte": [
                            "$l_shipdate",
                            {
                                "$dateSubtract": {
                                    "startDate": new ISODate("1998-12-01"),
                                    "unit": "day",
                                    "amount": 90,
                                },
                            },
                        ],
                    },
                },
            },
            {
                "$group": {
                    "_id": {"l_returnflag": "$l_returnflag", "l_linestatus": "$l_linestatus"},
                    "sum_qty": {"$sum": "$l_quantity"},
                    "sum_base_price": {"$sum": "$l_extendedprice"},
                    "sum_disc_price": {
                        "$sum": {
                            "$multiply": ["$l_extendedprice", {"$subtract": [1, "$l_discount"]}],
                        },
                    },
                    "sum_charge": {
                        "$sum": {
                            "$multiply": [
                                "$l_extendedprice",
                                {"$subtract": [1, "$l_discount"]},
                                {"$add": [1, "$l_tax"]},
                            ],
                        },
                    },
                    "avg_qty": {"$avg": "$l_quantity"},
                    "avg_price": {"$avg": "$l_extendedprice"},
                    "avg_disc": {"$avg": "$l_discount"},
                    "count_order": {"$count": {}},
                },
            },
            {
                "$project": {
                    "_id": 0,
                    "l_returnflag": "$_id.l_returnflag",
                    "l_linestatus": "$_id.l_linestatus",
                    "sum_qty": 1,
                    "sum_base_price": 1,
                    "sum_disc_price": 1,
                    "sum_charge": 1,
                    "avg_qty": 1,
                    "avg_price": 1,
                    "avg_disc": 1,
                    "count_order": 1,
                },
            },
            {"$sort": {"l_returnflag": 1, "l_linestatus": 1}},
        ],
    },

    {
        "idx": "3",
        "aggregate": "customer",
        "pipeline": [
            {"$match": {"$expr": {"$eq": ["$c_mktsegment", "BUILDING"]}}},
            {
                "$lookup": {
                    "from": "orders",
                    "localField": "c_custkey",
                    "foreignField": "o_custkey",
                    "as": "orders",
                    "pipeline": [
                        {
                            "$match": {
                                "$expr": {
                                    "$lt": ["$o_orderdate", new ISODate("1995-03-15")],
                                },
                            },
                        },
                    ],
                },
            },
            {"$unwind": "$orders"},
            {
                "$lookup": {
                    "from": "lineitem",
                    "localField": "orders.o_orderkey",
                    "foreignField": "l_orderkey",
                    "as": "lineitem",
                    "pipeline": [
                        {
                            "$match": {
                                "$expr": {
                                    "$gt": ["$l_shipdate", new ISODate("1995-03-15")],
                                },
                            },
                        },
                    ],
                },
            },
            {"$unwind": "$lineitem"},
            {
                "$group": {
                    "_id": {
                        "l_orderkey": "$lineitem.l_orderkey",
                        "o_orderdate": "$orders.o_orderdate",
                        "o_shippriority": "$orders.o_shippriority",
                    },
                    "revenue": {
                        "$sum": {
                            "$multiply": ["$lineitem.l_extendedprice", {"$subtract": [1, "$lineitem.l_discount"]}],
                        },
                    },
                },
            },
            {
                "$project": {
                    "_id": 0,
                    "l_orderkey": "$_id.l_orderkey",
                    "o_orderdate": "$_id.o_orderdate",
                    "o_shippriority": "$_id.o_shippriority",
                    "revenue": 1,
                },
            },
            {"$sort": {"revenue": -1, "o_orderdate": 1}},
            {"$limit": 10},
        ],
    },

    {
        "idx": "5",
        "aggregate": "customer",
        "pipeline": [
            {
                "$lookup": {
                    "from": "orders",
                    "localField": "c_custkey",
                    "foreignField": "o_custkey",
                    "as": "orders",
                    "pipeline": [
                        {
                            "$match": {
                                "$and": [
                                    {
                                        "$expr": {
                                            "$gte": ["$o_orderdate", new ISODate("1994-01-01")],
                                        },
                                    },
                                    {
                                        "$expr": {
                                            "$lt": [
                                                "$o_orderdate",
                                                {
                                                    "$dateAdd": {
                                                        "startDate": new ISODate("1994-01-01"),
                                                        "unit": "year",
                                                        "amount": 1,
                                                    },
                                                },
                                            ],
                                        },
                                    },
                                ],
                            },
                        },
                    ],
                },
            },
            {"$unwind": "$orders"},
            {
                "$lookup": {
                    "from": "lineitem",
                    "localField": "orders.o_orderkey",
                    "foreignField": "l_orderkey",
                    "as": "lineitem",
                },
            },
            {"$unwind": "$lineitem"},
            {
                "$lookup": {
                    "from": "supplier",
                    "localField": "lineitem.l_suppkey",
                    "foreignField": "s_suppkey",
                    "as": "supplier",
                    "let": {"c_nationkey": "$c_nationkey"},
                    "pipeline": [{"$match": {"$expr": {"$eq": ["$s_nationkey", "$$c_nationkey"]}}}],
                },
            },
            {"$unwind": "$supplier"},
            {
                "$lookup": {
                    "from": "nation",
                    "localField": "supplier.s_nationkey",
                    "foreignField": "n_nationkey",
                    "as": "nation",
                },
            },
            {"$unwind": "$nation"},
            {
                "$lookup": {
                    "from": "region",
                    "localField": "nation.n_regionkey",
                    "foreignField": "r_regionkey",
                    "as": "region",
                    "pipeline": [{"$match": {"$expr": {"$eq": ["$r_name", "ASIA"]}}}],
                },
            },
            {"$unwind": "$region"},
            {
                "$group": {
                    "_id": "$nation.n_name",
                    "revenue": {
                        "$sum": {
                            "$multiply": ["$lineitem.l_extendedprice", {"$subtract": [1, "$lineitem.l_discount"]}],
                        },
                    },
                },
            },
            {"$project": {"_id": 0, "n_name": "$_id", "revenue": 1}},
            {"$sort": {"revenue": -1}},
        ],
    },

    {
        "idx": "7",
        "aggregate": "supplier",
        "pipeline": [
            {
                "$lookup": {
                    "from": "lineitem",
                    "localField": "s_suppkey",
                    "foreignField": "l_suppkey",
                    "as": "lineitem",
                    "pipeline": [
                        {
                            "$match": {
                                "$and": [
                                    {
                                        "$expr": {
                                            "$gte": ["$l_shipdate", new ISODate("1995-01-01")],
                                        },
                                    },
                                    {
                                        "$expr": {
                                            "$lte": ["$l_shipdate", new ISODate("1996-12-31")],
                                        },
                                    },
                                ],
                            },
                        },
                    ],
                },
            },
            {"$unwind": "$lineitem"},
            {
                "$lookup": {
                    "from": "orders",
                    "localField": "lineitem.l_orderkey",
                    "foreignField": "o_orderkey",
                    "as": "orders",
                },
            },
            {"$unwind": "$orders"},
            {
                "$lookup": {
                    "from": "customer",
                    "localField": "orders.o_custkey",
                    "foreignField": "c_custkey",
                    "as": "customer",
                },
            },
            {"$unwind": "$customer"},
            {
                "$lookup": {
                    "from": "nation",
                    "localField": "s_nationkey",
                    "foreignField": "n_nationkey",
                    "as": "n1",
                },
            },
            {"$unwind": "$n1"},
            {
                "$lookup": {
                    "from": "nation",
                    "localField": "customer.c_nationkey",
                    "foreignField": "n_nationkey",
                    "as": "n2",
                    "let": {"n1_name": "$n1.n_name"},
                    "pipeline": [
                        {
                            "$match": {
                                "$expr": {
                                    "$or": [
                                        {
                                            "$and": [{"$eq": ["$$n1_name", "FRANCE"]}, {"$eq": ["$n_name", "GERMANY"]}],
                                        },
                                        {
                                            "$and": [{"$eq": ["$$n1_name", "GERMANY"]}, {"$eq": ["$n_name", "FRANCE"]}],
                                        },
                                    ],
                                },
                            },
                        },
                    ],
                },
            },
            {"$unwind": "$n2"},
            {
                "$project": {
                    "supp_nation": "$n1.n_name",
                    "cust_nation": "$n2.n_name",
                    "l_year": {"$year": "$lineitem.l_shipdate"},
                    "volume": {
                        "$multiply": ["$lineitem.l_extendedprice", {"$subtract": [1, "$lineitem.l_discount"]}],
                    },
                },
            },
            {
                "$group": {
                    "_id": {
                        "supp_nation": "$supp_nation",
                        "cust_nation": "$cust_nation",
                        "l_year": "$l_year",
                    },
                    "revenue": {"$sum": "$volume"},
                },
            },
            {
                "$project": {
                    "_id": 0,
                    "supp_nation": "$_id.supp_nation",
                    "cust_nation": "$_id.cust_nation",
                    "l_year": "$_id.l_year",
                    "revenue": 1,
                },
            },
            {"$sort": {"supp_nation": 1, "cust_nation": 1, "l_year": 1}},
        ],
    },

    {
        "idx": "8",
        "aggregate": "part",
        "pipeline": [
            {"$match": {"$expr": {"$eq": ["$p_type", "ECONOMY ANODIZED STEEL"]}}},
            {
                "$lookup": {
                    "from": "supplier",
                    "as": "supplier",
                    "pipeline": [],
                },
            },
            {"$unwind": "$supplier"},
            {
                "$lookup": {
                    "from": "lineitem",
                    "localField": "p_partkey",
                    "foreignField": "l_partkey",
                    "as": "lineitem",
                    "let": {"s_suppkey": "$supplier.s_suppkey"},
                    "pipeline": [{"$match": {"$expr": {"$eq": ["$l_suppkey", "$$s_suppkey"]}}}],
                },
            },
            {"$unwind": "$lineitem"},
            {
                "$lookup": {
                    "from": "orders",
                    "localField": "lineitem.l_orderkey",
                    "foreignField": "o_orderkey",
                    "as": "orders",
                    "pipeline": [
                        {
                            "$match": {
                                "$and": [
                                    {"$expr": {"$gte": ["$o_orderdate", new ISODate("1995-01-01")]}},
                                    {"$expr": {"$lte": ["$o_orderdate", new ISODate("1996-12-31")]}},
                                ],
                            },
                        },
                    ],
                },
            },
            {"$unwind": "$orders"},
            {
                "$lookup": {
                    "from": "customer",
                    "localField": "orders.o_custkey",
                    "foreignField": "c_custkey",
                    "as": "customer",
                },
            },
            {"$unwind": "$customer"},
            {
                "$lookup": {
                    "from": "nation",
                    "localField": "customer.c_nationkey",
                    "foreignField": "n_nationkey",
                    "as": "n1",
                },
            },
            {"$unwind": "$n1"},
            {
                "$lookup": {
                    "from": "nation",
                    "localField": "supplier.s_nationkey",
                    "foreignField": "n_nationkey",
                    "as": "n2",
                },
            },
            {"$unwind": "$n2"},
            {
                "$lookup": {
                    "from": "region",
                    "localField": "n1.n_regionkey",
                    "foreignField": "r_regionkey",
                    "as": "region",
                    "pipeline": [{"$match": {"$expr": {"$eq": ["$r_name", "AMERICA"]}}}],
                },
            },
            {"$unwind": "$region"},
            {
                "$project": {
                    "o_year": {"$year": "$orders.o_orderdate"},
                    "volume": {
                        "$multiply": [
                            "$lineitem.l_extendedprice",
                            {"$subtract": [{"$literal": 1}, "$lineitem.l_discount"]},
                        ],
                    },
                    "nation": "$n2.n_name",
                },
            },
            {
                "$group": {
                    "_id": "$o_year",
                    "total_volume": {"$sum": "$volume"},
                    "nation_volume": {
                        "$sum": {
                            "$cond": {
                                "if": {"$eq": ["$nation", "BRAZIL"]},
                                "then": "$volume",
                                "else": 0,
                            },
                        },
                    },
                },
            },
            {
                "$project": {
                    "_id": 0,
                    "o_year": "$_id",
                    "mkt_share": {"$divide": ["$nation_volume", "$total_volume"]},
                },
            },
            {"$sort": {"o_year": 1}},
        ],
    },

    {
        "idx": "9",
        "aggregate": "part",
        "pipeline": [
            {"$match": {"p_name": {"$regex": "green"}}},
            {
                "$lookup": {
                    "from": "supplier",
                    "as": "supplier",
                    "pipeline": [],
                },
            },
            {"$unwind": "$supplier"},
            {
                "$lookup": {
                    "from": "lineitem",
                    "localField": "supplier.s_suppkey",
                    "foreignField": "l_suppkey",
                    "as": "lineitem",
                    "let": {"p_partkey": "$p_partkey"},
                    "pipeline": [{"$match": {"$expr": {"$eq": ["$l_partkey", "$$p_partkey"]}}}],
                },
            },
            {"$unwind": "$lineitem"},
            {
                "$lookup": {
                    "from": "partsupp",
                    "localField": "lineitem.l_suppkey",
                    "foreignField": "ps_suppkey",
                    "let": {"l_partkey": "$lineitem.l_partkey"},
                    "as": "partsupp",
                    "pipeline": [{"$match": {"$expr": {"$eq": ["$ps_partkey", "$$l_partkey"]}}}],
                },
            },
            {"$unwind": "$partsupp"},
            {
                "$lookup": {
                    "from": "orders",
                    "localField": "lineitem.l_orderkey",
                    "foreignField": "o_orderkey",
                    "as": "orders",
                },
            },
            {"$unwind": "$orders"},
            {
                "$lookup": {
                    "from": "nation",
                    "localField": "supplier.s_nationkey",
                    "foreignField": "n_nationkey",
                    "as": "nation",
                },
            },
            {"$unwind": "$nation"},
            {
                "$project": {
                    "nation": "$nation.n_name",
                    "o_year": {"$year": "$orders.o_orderdate"},
                    "amount": {
                        "$subtract": [
                            {
                                "$multiply": ["$lineitem.l_extendedprice", {"$subtract": [1, "$lineitem.l_discount"]}],
                            },
                            {
                                "$multiply": ["$partsupp.ps_supplycost", "$lineitem.l_quantity"],
                            },
                        ],
                    },
                },
            },
            {
                "$group": {
                    "_id": {
                        "nation": "$nation",
                        "o_year": "$o_year",
                    },
                    "sum_profit": {"$sum": "$amount"},
                },
            },
            {
                "$project": {
                    "_id": 0,
                    "nation": "$_id.nation",
                    "o_year": "$_id.o_year",
                    "sum_profit": 1,
                },
            },
            {"$sort": {"nation": 1, "o_year": -1}},
        ],
    },

    {
        "idx": "10",
        "aggregate": "customer",
        "pipeline": [
            {
                "$lookup": {
                    "from": "orders",
                    "localField": "c_custkey",
                    "foreignField": "o_custkey",
                    "as": "orders",
                    "pipeline": [
                        {
                            "$match": {
                                "$and": [
                                    {
                                        "$expr": {
                                            "$gte": ["$o_orderdate", new ISODate("1993-10-01")],
                                        },
                                    },
                                    {
                                        "$expr": {
                                            "$lt": [
                                                "$o_orderdate",
                                                {
                                                    "$dateAdd": {
                                                        "startDate": new ISODate("1993-10-01"),
                                                        "unit": "month",
                                                        "amount": 3,
                                                    },
                                                },
                                            ],
                                        },
                                    },
                                ],
                            },
                        },
                    ],
                },
            },
            {"$unwind": "$orders"},
            {
                "$lookup": {
                    "from": "lineitem",
                    "localField": "orders.o_orderkey",
                    "foreignField": "l_orderkey",
                    "as": "lineitem",
                    "pipeline": [{"$match": {"$expr": {"$eq": ["$l_returnflag", "R"]}}}],
                },
            },
            {"$unwind": "$lineitem"},
            {
                "$lookup": {
                    "from": "nation",
                    "localField": "c_nationkey",
                    "foreignField": "n_nationkey",
                    "as": "nation",
                },
            },
            {"$unwind": "$nation"},
            {
                "$group": {
                    "_id": {
                        "c_custkey": "$c_custkey",
                        "c_name": "$c_name",
                        "c_acctbal": "$c_acctbal",
                        "c_phone": "$c_phone",
                        "n_name": "$nation.n_name",
                        "c_address": "$c_address",
                        "c_comment": "$c_comment",
                    },
                    "revenue": {
                        "$sum": {
                            "$multiply": ["$lineitem.l_extendedprice", {"$subtract": [1, "$lineitem.l_discount"]}],
                        },
                    },
                },
            },
            {
                "$project": {
                    "_id": 0,
                    "c_custkey": "$_id.c_custkey",
                    "c_name": "$_id.c_name",
                    "revenue": 1,
                    "c_acctbal": "$_id.c_acctbal",
                    "n_name": "$_id.n_name",
                    "c_address": "$_id.c_address",
                    "c_phone": "$_id.c_phone",
                    "c_comment": "$_id.c_comment",
                },
            },
            {"$sort": {"revenue": -1}},
            {"$limit": 20},
        ],
    },

    {
        "idx": "12",
        "aggregate": "orders",
        "pipeline": [
            {
                "$lookup": {
                    "from": "lineitem",
                    "as": "lineitem",
                    "localField": "o_orderkey",
                    "foreignField": "l_orderkey",
                    "pipeline": [
                        {
                            "$match": {
                                "$and": [
                                    {"$expr": {"$in": ["$l_shipmode", ["MAIL", "SHIP"]]}},
                                    {"$expr": {"$lt": ["$l_commitdate", "$l_receiptdate"]}},
                                    {"$expr": {"$lt": ["$l_shipdate", "$l_commitdate"]}},
                                    {
                                        "$expr": {
                                            "$gte": ["$l_receiptdate", new ISODate("1994-01-01")],
                                        },
                                    },
                                    {
                                        "$expr": {
                                            "$lt": [
                                                "$l_receiptdate",
                                                {
                                                    "$dateAdd": {
                                                        "startDate": new ISODate("1994-01-01"),
                                                        "unit": "year",
                                                        "amount": 1,
                                                    },
                                                },
                                            ],
                                        },
                                    },
                                ],
                            },
                        },
                    ],
                },
            },
            {"$unwind": "$lineitem"},
            {
                "$group": {
                    "_id": "$lineitem.l_shipmode",
                    "high_line_count": {
                        "$sum": {
                            "$cond": {
                                "if": {
                                    "$or": [
                                        {
                                            "$eq": ["$o_orderpriority", "1-URGENT"],
                                        },
                                        {"$eq": ["$o_orderpriority", "2-HIGH"]},
                                    ],
                                },
                                "then": 1,
                                "else": 0,
                            },
                        },
                    },
                    "low_line_count": {
                        "$sum": {
                            "$cond": {
                                "if": {
                                    "$and": [
                                        {
                                            "$ne": ["$o_orderpriority", "1-URGENT"],
                                        },
                                        {"$ne": ["$o_orderpriority", "2-HIGH"]},
                                    ],
                                },
                                "then": 1,
                                "else": 0,
                            },
                        },
                    },
                },
            },
            {
                "$project": {
                    "_id": 0,
                    "l_shipmode": "$_id",
                    "high_line_count": 1,
                    "low_line_count": 1,
                },
            },
            {"$sort": {"l_shipmode": 1}},
        ],
    },

    {
        "idx": "14",
        "aggregate": "lineitem",
        "pipeline": [
            {
                "$match": {
                    "$and": [
                        {"$expr": {"$gte": ["$l_shipdate", new ISODate("1995-09-01")]}},
                        {
                            "$expr": {
                                "$lt": [
                                    "$l_shipdate",
                                    {
                                        "$dateAdd": {
                                            "startDate": new ISODate("1995-09-01"),
                                            "unit": "month",
                                            "amount": 1,
                                        },
                                    },
                                ],
                            },
                        },
                    ],
                },
            },
            {
                "$lookup": {
                    "from": "part",
                    "as": "part",
                    "localField": "l_partkey",
                    "foreignField": "p_partkey",
                },
            },
            {"$unwind": "$part"},
            {
                "$group": {
                    "_id": {},
                    "promo_price_total": {
                        "$sum": {
                            "$cond": {
                                "if": {
                                    "$cond": {
                                        "if": {
                                            "$regexMatch": {
                                                "input": "$part.p_type",
                                                "regex": "^PROMO.*$",
                                                "options": "si",
                                            },
                                        },
                                        "then": 1,
                                        "else": 0,
                                    },
                                },
                                "then": {
                                    "$multiply": ["$l_extendedprice", {"$subtract": [1, "$l_discount"]}],
                                },
                                "else": 0,
                            },
                        },
                    },
                    "price_total": {
                        "$sum": {
                            "$multiply": ["$l_extendedprice", {"$subtract": [1, "$l_discount"]}],
                        },
                    },
                },
            },
            {
                "$project": {
                    "_id": 0,
                    "promo_revenue": {
                        "$multiply": [100.0, {"$divide": ["$promo_price_total", "$price_total"]}],
                    },
                },
            },
        ],
    },
];
