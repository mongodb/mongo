## >>> Command idx 1
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$or":[{"ps_comment":{"$regex":{"$regex":"^ ab","$options":""}}},{"ps_supplycost":{"$lt":68.15}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"supplier","localField":"partsupp.ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$or":[{"s_name":"Supplier#000000255"},{"s_acctbal":{"$lte":7307.62}}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$or":[{"n_name":"BRAZIL"},{"n_name":{"$nin":["CHINA","GERMANY"]}}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$lookup":{"from":"region","localField":"nation_s.n_regionkey","foreignField":"r_regionkey","pipeline":[
{"$match":{"$nor":[{"r_name":"EUROPE"},{"r_name":"AFRICA"}]}}],"as":"region_s"}},
{"$unwind":"$region_s"},
{"$match":{"$and":[{"p_type":"LARGE BURNISHED NICKEL"},{"p_retailprice":{"$gt":1699.79}}]}}],"cursor":{},"idx":1}
```
### >>> Subjoin 1-0
```
db.part.aggregate(EJSON.deserialize(
[
{"$match":{"$and":[{"p_type":{"$eq":"LARGE BURNISHED NICKEL"}},{"p_retailprice":{"$gt":1699.79}}]}}]
));
```
Subjoin plan:
```
COLLSCAN: plan_stability_subjoin_cardinality_md.part {"$and":[{"p_type":{"$eq":"LARGE BURNISHED NICKEL"}},{"p_retailprice":{"$gt":1699.79}}]}
```
Estimated cardinality: 0  
Actual cardinality: 29  
Orders of magnitude: 1

---
### >>> Subjoin 1-1
```
db.part.aggregate(EJSON.deserialize(
[
{"$match":{"$and":[{"p_type":{"$eq":"LARGE BURNISHED NICKEL"}},{"p_retailprice":{"$gt":1699.79}}]}},
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","as":"partsupp","pipeline":[
{"$match":{"$and":[{"$or":[{"ps_supplycost":{"$lt":68.15}},{"ps_comment":{"$regex":"^ ab"}}]},{}]}}]}},
{"$unwind":"$partsupp"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$partsupp"]}}}]
));
```
Subjoin plan:
```
INLJ p_partkey = ps_partkey
  -> [none] COLLSCAN: plan_stability_subjoin_cardinality_md.part {"$and":[{"p_type":{"$eq":"LARGE BURNISHED NICKEL"}},{"p_retailprice":{"$gt":1699.79}}]} 
  -> [partsupp] FETCH: plan_stability_subjoin_cardinality_md.partsupp {"$or":[{"ps_supplycost":{"$lt":68.15}},{"ps_comment":{"$regex":"^ ab"}}]} 
      -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.partsupp ps_partkey_1
```
Estimated cardinality: 0  
Actual cardinality: 8  
Orders of magnitude: 0

---
### >>> Subjoin 1-2
```
db.part.aggregate(EJSON.deserialize(
[
{"$match":{"$and":[{"p_type":{"$eq":"LARGE BURNISHED NICKEL"}},{"p_retailprice":{"$gt":1699.79}}]}},
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","as":"partsupp","pipeline":[
{"$match":{"$and":[{"$or":[{"ps_supplycost":{"$lt":68.15}},{"ps_comment":{"$regex":"^ ab"}}]},{}]}}]}},
{"$unwind":"$partsupp"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$partsupp"]}}},
{"$lookup":{"from":"supplier","localField":"ps_suppkey","foreignField":"s_suppkey","as":"supplier","pipeline":[
{"$match":{"$and":[{"$or":[{"s_name":{"$eq":"Supplier#000000255"}},{"s_acctbal":{"$lte":7307.62}}]},{}]}}]}},
{"$unwind":"$supplier"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$supplier"]}}}]
));
```
Subjoin plan:
```
INLJ partsupp.ps_suppkey = s_suppkey
  -> [none] INLJ p_partkey = ps_partkey
      -> [none] COLLSCAN: plan_stability_subjoin_cardinality_md.part {"$and":[{"p_type":{"$eq":"LARGE BURNISHED NICKEL"}},{"p_retailprice":{"$gt":1699.79}}]} 
      -> [partsupp] FETCH: plan_stability_subjoin_cardinality_md.partsupp {"$or":[{"ps_supplycost":{"$lt":68.15}},{"ps_comment":{"$regex":"^ ab"}}]} 
          -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.partsupp ps_partkey_1
  -> [supplier] FETCH: plan_stability_subjoin_cardinality_md.supplier {"$or":[{"s_name":{"$eq":"Supplier#000000255"}},{"s_acctbal":{"$lte":7307.62}}]} 
      -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.supplier s_suppkey_1
```
Estimated cardinality: 0  
Actual cardinality: 4  
Orders of magnitude: 0

---
### >>> Subjoin 1-3
```
db.part.aggregate(EJSON.deserialize(
[
{"$match":{"$and":[{"p_type":{"$eq":"LARGE BURNISHED NICKEL"}},{"p_retailprice":{"$gt":1699.79}}]}},
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","as":"partsupp","pipeline":[
{"$match":{"$and":[{"$or":[{"ps_supplycost":{"$lt":68.15}},{"ps_comment":{"$regex":"^ ab"}}]},{}]}}]}},
{"$unwind":"$partsupp"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$partsupp"]}}},
{"$lookup":{"from":"supplier","localField":"ps_suppkey","foreignField":"s_suppkey","as":"supplier","pipeline":[
{"$match":{"$and":[{"$or":[{"s_name":{"$eq":"Supplier#000000255"}},{"s_acctbal":{"$lte":7307.62}}]},{}]}}]}},
{"$unwind":"$supplier"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$supplier"]}}},
{"$lookup":{"from":"nation","localField":"s_nationkey","foreignField":"n_nationkey","as":"nation","pipeline":[
{"$match":{"$and":[{"$or":[{"n_name":{"$eq":"BRAZIL"}},{"n_name":{"$not":{"$in":["CHINA","GERMANY"]}}}]},{}]}}]}},
{"$unwind":"$nation"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$nation"]}}}]
));
```
Subjoin plan:
```
INLJ supplier.s_nationkey = n_nationkey
  -> [none] INLJ partsupp.ps_suppkey = s_suppkey
      -> [none] INLJ p_partkey = ps_partkey
          -> [none] COLLSCAN: plan_stability_subjoin_cardinality_md.part {"$and":[{"p_type":{"$eq":"LARGE BURNISHED NICKEL"}},{"p_retailprice":{"$gt":1699.79}}]} 
          -> [partsupp] FETCH: plan_stability_subjoin_cardinality_md.partsupp {"$or":[{"ps_supplycost":{"$lt":68.15}},{"ps_comment":{"$regex":"^ ab"}}]} 
              -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.partsupp ps_partkey_1
      -> [supplier] FETCH: plan_stability_subjoin_cardinality_md.supplier {"$or":[{"s_name":{"$eq":"Supplier#000000255"}},{"s_acctbal":{"$lte":7307.62}}]} 
          -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.supplier s_suppkey_1
  -> [nation_s] FETCH: plan_stability_subjoin_cardinality_md.nation {"$or":[{"n_name":{"$eq":"BRAZIL"}},{"n_name":{"$not":{"$in":["CHINA","GERMANY"]}}}]} 
      -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.nation n_nationkey_1
```
Estimated cardinality: 0  
Actual cardinality: 4  
Orders of magnitude: 0

---
### >>> Subjoin 1-4
```
db.part.aggregate(EJSON.deserialize(
[
{"$match":{"$and":[{"p_type":{"$eq":"LARGE BURNISHED NICKEL"}},{"p_retailprice":{"$gt":1699.79}}]}},
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","as":"partsupp","pipeline":[
{"$match":{"$and":[{"$or":[{"ps_supplycost":{"$lt":68.15}},{"ps_comment":{"$regex":"^ ab"}}]},{}]}}]}},
{"$unwind":"$partsupp"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$partsupp"]}}},
{"$lookup":{"from":"supplier","localField":"ps_suppkey","foreignField":"s_suppkey","as":"supplier","pipeline":[
{"$match":{"$and":[{"$or":[{"s_name":{"$eq":"Supplier#000000255"}},{"s_acctbal":{"$lte":7307.62}}]},{}]}}]}},
{"$unwind":"$supplier"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$supplier"]}}},
{"$lookup":{"from":"nation","localField":"s_nationkey","foreignField":"n_nationkey","as":"nation","pipeline":[
{"$match":{"$and":[{"$or":[{"n_name":{"$eq":"BRAZIL"}},{"n_name":{"$not":{"$in":["CHINA","GERMANY"]}}}]},{}]}}]}},
{"$unwind":"$nation"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$nation"]}}},
{"$lookup":{"from":"region","localField":"n_regionkey","foreignField":"r_regionkey","as":"region","pipeline":[
{"$match":{"$and":[{"$nor":[{"r_name":{"$eq":"EUROPE"}},{"r_name":{"$eq":"AFRICA"}}]},{}]}}]}},
{"$unwind":"$region"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$region"]}}}]
));
```
Subjoin plan:
```
INLJ nation_s.n_regionkey = r_regionkey
  -> [none] INLJ supplier.s_nationkey = n_nationkey
      -> [none] INLJ partsupp.ps_suppkey = s_suppkey
          -> [none] INLJ p_partkey = ps_partkey
              -> [none] COLLSCAN: plan_stability_subjoin_cardinality_md.part {"$and":[{"p_type":{"$eq":"LARGE BURNISHED NICKEL"}},{"p_retailprice":{"$gt":1699.79}}]} 
              -> [partsupp] FETCH: plan_stability_subjoin_cardinality_md.partsupp {"$or":[{"ps_supplycost":{"$lt":68.15}},{"ps_comment":{"$regex":"^ ab"}}]} 
                  -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.partsupp ps_partkey_1
          -> [supplier] FETCH: plan_stability_subjoin_cardinality_md.supplier {"$or":[{"s_name":{"$eq":"Supplier#000000255"}},{"s_acctbal":{"$lte":7307.62}}]} 
              -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.supplier s_suppkey_1
      -> [nation_s] FETCH: plan_stability_subjoin_cardinality_md.nation {"$or":[{"n_name":{"$eq":"BRAZIL"}},{"n_name":{"$not":{"$in":["CHINA","GERMANY"]}}}]} 
          -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.nation n_nationkey_1
  -> [region_s] FETCH: plan_stability_subjoin_cardinality_md.region {"$nor":[{"r_name":{"$eq":"EUROPE"}},{"r_name":{"$eq":"AFRICA"}}]} 
      -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.region r_regionkey_1
```
Estimated cardinality: 0  
Actual cardinality: 3  
Orders of magnitude: 0

---
## >>> Command idx 2
```
{"aggregate":"supplier","pipeline":[
{"$lookup":{"from":"nation","localField":"s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$nor":[{"n_regionkey":2}]}},
{"$match":{"$nor":[{"n_regionkey":1},{"n_regionkey":1},{"n_regionkey":0}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$lookup":{"from":"region","localField":"nation_s.n_regionkey","foreignField":"r_regionkey","pipeline":[
{"$match":{"$nor":[{"r_name":"ASIA"},{"r_name":"AFRICA"}]}},
{"$match":{"$nor":[{"r_regionkey":4}]}}],"as":"region_s"}},
{"$unwind":"$region_s"},
{"$match":{"$nor":[{"region_s.r_name":"AFRICA"},{"region_s.r_name":"AMERICA"}]}}],"cursor":{},"idx":2}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 3
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$nor":[{"ps_supplycost":{"$eq":12.1}},{"ps_comment":{"$regex":{"$regex":"^hins","$options":""}}}]}},
{"$match":{"$or":[{"ps_supplycost":{"$gt":662.79}},{"ps_supplycost":{"$eq":309.37}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"supplier","localField":"partsupp.ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$and":[{"s_acctbal":{"$gte":2697.53}},{"s_name":"Supplier#000000264"}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$or":[{"n_regionkey":3}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$match":{"$or":[{"supplier.s_acctbal":{"$gt":1050.66}},{"nation_s.n_regionkey":2}]}}],"cursor":{},"idx":3}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 4
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$and":[{"ps_comment":{"$regex":{"$regex":"^l","$options":""}}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"supplier","localField":"partsupp.ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$and":[{"s_name":"Supplier#000000749"}]}},
{"$match":{"$and":[{"s_acctbal":{"$lt":7182.24}}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$nor":[{"n_regionkey":1},{"n_regionkey":4}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$lookup":{"from":"region","localField":"nation_s.n_regionkey","foreignField":"r_regionkey","pipeline":[
{"$match":{"$and":[{"r_regionkey":0}]}}],"as":"region_s"}},
{"$unwind":"$region_s"},
{"$match":{"$and":[{"p_mfgr":"Manufacturer#1"}]}}],"cursor":{},"idx":4}
```
### >>> Subjoin 4-0
```
db.supplier.aggregate(EJSON.deserialize(
[
{"$match":{"$and":[{"s_name":{"$eq":"Supplier#000000749"}},{"s_acctbal":{"$lt":7182.24}}]}}]
));
```
Subjoin plan:
```
COLLSCAN: plan_stability_subjoin_cardinality_md.supplier {"$and":[{"s_name":{"$eq":"Supplier#000000749"}},{"s_acctbal":{"$lt":7182.24}}]}
```
Estimated cardinality: 1  
Actual cardinality: 1  
Orders of magnitude: 0

---
### >>> Subjoin 4-1
```
db.supplier.aggregate(EJSON.deserialize(
[
{"$match":{"$and":[{"s_name":{"$eq":"Supplier#000000749"}},{"s_acctbal":{"$lt":7182.24}}]}},
{"$lookup":{"from":"nation","localField":"s_nationkey","foreignField":"n_nationkey","as":"nation","pipeline":[
{"$match":{"$nor":[{"n_regionkey":{"$eq":1}},{"n_regionkey":{"$eq":4}}]}}]}},
{"$unwind":"$nation"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$nation"]}}}]
));
```
Subjoin plan:
```
NLJ s_nationkey = n_nationkey
  -> [supplier] COLLSCAN: plan_stability_subjoin_cardinality_md.supplier {"$and":[{"s_name":{"$eq":"Supplier#000000749"}},{"s_acctbal":{"$lt":7182.24}}]} 
  -> [nation_s] COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"$nor":[{"n_regionkey":{"$eq":1}},{"n_regionkey":{"$eq":4}}]}
```
Estimated cardinality: 1  
Actual cardinality: 1  
Orders of magnitude: 0

---
### >>> Subjoin 4-2
```
db.supplier.aggregate(EJSON.deserialize(
[
{"$match":{"$and":[{"s_name":{"$eq":"Supplier#000000749"}},{"s_acctbal":{"$lt":7182.24}}]}},
{"$lookup":{"from":"nation","localField":"s_nationkey","foreignField":"n_nationkey","as":"nation","pipeline":[
{"$match":{"$nor":[{"n_regionkey":{"$eq":1}},{"n_regionkey":{"$eq":4}}]}}]}},
{"$unwind":"$nation"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$nation"]}}},
{"$lookup":{"from":"region","localField":"n_regionkey","foreignField":"r_regionkey","as":"region","pipeline":[
{"$match":{"$and":[{"r_regionkey":{"$eq":0}},{}]}}]}},
{"$unwind":"$region"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$region"]}}}]
));
```
Subjoin plan:
```
INLJ nation_s.n_regionkey = r_regionkey
  -> [none] NLJ s_nationkey = n_nationkey
      -> [supplier] COLLSCAN: plan_stability_subjoin_cardinality_md.supplier {"$and":[{"s_name":{"$eq":"Supplier#000000749"}},{"s_acctbal":{"$lt":7182.24}}]} 
      -> [nation_s] COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"$nor":[{"n_regionkey":{"$eq":1}},{"n_regionkey":{"$eq":4}}]} 
  -> [region_s] FETCH: plan_stability_subjoin_cardinality_md.region {"r_regionkey":{"$eq":0}} 
      -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.region r_regionkey_1
```
Estimated cardinality: 0  
Actual cardinality: 1  
Orders of magnitude: 0

---
### >>> Subjoin 4-3
```
db.supplier.aggregate(EJSON.deserialize(
[
{"$match":{"$and":[{"s_name":{"$eq":"Supplier#000000749"}},{"s_acctbal":{"$lt":7182.24}}]}},
{"$lookup":{"from":"nation","localField":"s_nationkey","foreignField":"n_nationkey","as":"nation","pipeline":[
{"$match":{"$nor":[{"n_regionkey":{"$eq":1}},{"n_regionkey":{"$eq":4}}]}}]}},
{"$unwind":"$nation"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$nation"]}}},
{"$lookup":{"from":"region","localField":"n_regionkey","foreignField":"r_regionkey","as":"region","pipeline":[
{"$match":{"$and":[{"r_regionkey":{"$eq":0}},{}]}}]}},
{"$unwind":"$region"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$region"]}}},
{"$lookup":{"from":"partsupp","localField":"s_suppkey","foreignField":"ps_suppkey","as":"partsupp","pipeline":[
{"$match":{"$and":[{"ps_comment":{"$regex":"^l"}},{}]}}]}},
{"$unwind":"$partsupp"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$partsupp"]}}}]
));
```
Subjoin plan:
```
INLJ supplier.s_suppkey = ps_suppkey
  -> [none] INLJ nation_s.n_regionkey = r_regionkey
      -> [none] NLJ s_nationkey = n_nationkey
          -> [supplier] COLLSCAN: plan_stability_subjoin_cardinality_md.supplier {"$and":[{"s_name":{"$eq":"Supplier#000000749"}},{"s_acctbal":{"$lt":7182.24}}]} 
          -> [nation_s] COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"$nor":[{"n_regionkey":{"$eq":1}},{"n_regionkey":{"$eq":4}}]} 
      -> [region_s] FETCH: plan_stability_subjoin_cardinality_md.region {"r_regionkey":{"$eq":0}} 
          -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.region r_regionkey_1
  -> [partsupp] FETCH: plan_stability_subjoin_cardinality_md.partsupp {"ps_comment":{"$regex":"^l"}} 
      -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.partsupp ps_suppkey_1
```
Estimated cardinality: 1  
Actual cardinality: 7  
Orders of magnitude: 0

---
### >>> Subjoin 4-4
```
db.supplier.aggregate(EJSON.deserialize(
[
{"$match":{"$and":[{"s_name":{"$eq":"Supplier#000000749"}},{"s_acctbal":{"$lt":7182.24}}]}},
{"$lookup":{"from":"nation","localField":"s_nationkey","foreignField":"n_nationkey","as":"nation","pipeline":[
{"$match":{"$nor":[{"n_regionkey":{"$eq":1}},{"n_regionkey":{"$eq":4}}]}}]}},
{"$unwind":"$nation"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$nation"]}}},
{"$lookup":{"from":"region","localField":"n_regionkey","foreignField":"r_regionkey","as":"region","pipeline":[
{"$match":{"$and":[{"r_regionkey":{"$eq":0}},{}]}}]}},
{"$unwind":"$region"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$region"]}}},
{"$lookup":{"from":"partsupp","localField":"s_suppkey","foreignField":"ps_suppkey","as":"partsupp","pipeline":[
{"$match":{"$and":[{"ps_comment":{"$regex":"^l"}},{}]}}]}},
{"$unwind":"$partsupp"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$partsupp"]}}},
{"$lookup":{"from":"part","localField":"ps_partkey","foreignField":"p_partkey","as":"part","pipeline":[
{"$match":{"$and":[{"p_mfgr":{"$eq":"Manufacturer#1"}},{}]}}]}},
{"$unwind":"$part"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$part"]}}}]
));
```
Subjoin plan:
```
INLJ partsupp.ps_partkey = p_partkey
  -> [none] INLJ supplier.s_suppkey = ps_suppkey
      -> [none] INLJ nation_s.n_regionkey = r_regionkey
          -> [none] NLJ s_nationkey = n_nationkey
              -> [supplier] COLLSCAN: plan_stability_subjoin_cardinality_md.supplier {"$and":[{"s_name":{"$eq":"Supplier#000000749"}},{"s_acctbal":{"$lt":7182.24}}]} 
              -> [nation_s] COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"$nor":[{"n_regionkey":{"$eq":1}},{"n_regionkey":{"$eq":4}}]} 
          -> [region_s] FETCH: plan_stability_subjoin_cardinality_md.region {"r_regionkey":{"$eq":0}} 
              -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.region r_regionkey_1
      -> [partsupp] FETCH: plan_stability_subjoin_cardinality_md.partsupp {"ps_comment":{"$regex":"^l"}} 
          -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.partsupp ps_suppkey_1
  -> [none] FETCH: plan_stability_subjoin_cardinality_md.part {"p_mfgr":{"$eq":"Manufacturer#1"}} 
      -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.part p_partkey_1
```
Estimated cardinality: 0  
Actual cardinality: 1  
Orders of magnitude: 0

---
## >>> Command idx 5
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$or":[{"ps_availqty":{"$eq":5432}},{"ps_supplycost":{"$gt":941.7}}]}},
{"$match":{"$or":[{"ps_availqty":{"$eq":2021}},{"ps_availqty":{"$gte":3530}},{"ps_availqty":{"$gt":5420}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"supplier","localField":"partsupp.ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$nor":[{"s_acctbal":{"$eq":6399.78}}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$nor":[{"n_regionkey":1}]}},
{"$match":{"$or":[{"n_regionkey":4}]}},
{"$match":{"$nor":[{"n_name":"VIETNAM"},{"n_name":"UNITED KINGDOM"}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$lookup":{"from":"region","localField":"nation_s.n_regionkey","foreignField":"r_regionkey","pipeline":[
{"$match":{"$nor":[{"r_regionkey":0},{"r_regionkey":3}]}}],"as":"region_s"}},
{"$unwind":"$region_s"},
{"$match":{"$or":[{"p_name":{"$regex":{"$regex":"^for","$options":""}}}]}}],"cursor":{},"idx":5}
```
### >>> Subjoin 5-0
```
db.region.aggregate(EJSON.deserialize(
[
{"$match":{"$nor":[{"r_regionkey":{"$eq":0}},{"r_regionkey":{"$eq":3}}]}}]
));
```
Subjoin plan:
```
COLLSCAN: plan_stability_subjoin_cardinality_md.region {"$nor":[{"r_regionkey":{"$eq":0}},{"r_regionkey":{"$eq":3}}]}
```
Estimated cardinality: 3  
Actual cardinality: 3  
Orders of magnitude: 0

---
### >>> Subjoin 5-1
```
db.region.aggregate(EJSON.deserialize(
[
{"$match":{"$nor":[{"r_regionkey":{"$eq":0}},{"r_regionkey":{"$eq":3}}]}},
{"$lookup":{"from":"nation","localField":"r_regionkey","foreignField":"n_regionkey","as":"nation","pipeline":[
{"$match":{"$and":[{"$and":[{"n_regionkey":{"$eq":4}},{"n_regionkey":{"$not":{"$eq":1}}},{"$nor":[{"n_name":{"$eq":"VIETNAM"}},{"n_name":{"$eq":"UNITED KINGDOM"}}]}]},{}]}}]}},
{"$unwind":"$nation"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$nation"]}}}]
));
```
Subjoin plan:
```
INLJ r_regionkey = n_regionkey
  -> [region_s] COLLSCAN: plan_stability_subjoin_cardinality_md.region {"$nor":[{"r_regionkey":{"$eq":0}},{"r_regionkey":{"$eq":3}}]} 
  -> [nation_s] FETCH: plan_stability_subjoin_cardinality_md.nation {"$and":[{"n_regionkey":{"$eq":4}},{"n_regionkey":{"$not":{"$eq":1}}},{"$nor":[{"n_name":{"$eq":"VIETNAM"}},{"n_name":{"$eq":"UNITED KINGDOM"}}]}]} 
      -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.nation n_regionkey_1
```
Estimated cardinality: 3  
Actual cardinality: 5  
Orders of magnitude: 0

---
### >>> Subjoin 5-2
```
db.region.aggregate(EJSON.deserialize(
[
{"$match":{"$nor":[{"r_regionkey":{"$eq":0}},{"r_regionkey":{"$eq":3}}]}},
{"$lookup":{"from":"nation","localField":"r_regionkey","foreignField":"n_regionkey","as":"nation","pipeline":[
{"$match":{"$and":[{"$and":[{"n_regionkey":{"$eq":4}},{"n_regionkey":{"$not":{"$eq":1}}},{"$nor":[{"n_name":{"$eq":"VIETNAM"}},{"n_name":{"$eq":"UNITED KINGDOM"}}]}]},{}]}}]}},
{"$unwind":"$nation"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$nation"]}}},
{"$lookup":{"from":"supplier","localField":"n_nationkey","foreignField":"s_nationkey","as":"supplier","pipeline":[
{"$match":{"$and":[{"s_acctbal":{"$not":{"$eq":6399.78}}},{}]}}]}},
{"$unwind":"$supplier"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$supplier"]}}}]
));
```
Subjoin plan:
```
INLJ nation_s.n_nationkey = s_nationkey
  -> [none] INLJ r_regionkey = n_regionkey
      -> [region_s] COLLSCAN: plan_stability_subjoin_cardinality_md.region {"$nor":[{"r_regionkey":{"$eq":0}},{"r_regionkey":{"$eq":3}}]} 
      -> [nation_s] FETCH: plan_stability_subjoin_cardinality_md.nation {"$and":[{"n_regionkey":{"$eq":4}},{"n_regionkey":{"$not":{"$eq":1}}},{"$nor":[{"n_name":{"$eq":"VIETNAM"}},{"n_name":{"$eq":"UNITED KINGDOM"}}]}]} 
          -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.nation n_regionkey_1
  -> [supplier] FETCH: plan_stability_subjoin_cardinality_md.supplier {"s_acctbal":{"$not":{"$eq":6399.78}}} 
      -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.supplier s_nationkey_1
```
Estimated cardinality: 120  
Actual cardinality: 198  
Orders of magnitude: 0

---
### >>> Subjoin 5-3
```
db.region.aggregate(EJSON.deserialize(
[
{"$match":{"$nor":[{"r_regionkey":{"$eq":0}},{"r_regionkey":{"$eq":3}}]}},
{"$lookup":{"from":"nation","localField":"r_regionkey","foreignField":"n_regionkey","as":"nation","pipeline":[
{"$match":{"$and":[{"$and":[{"n_regionkey":{"$eq":4}},{"n_regionkey":{"$not":{"$eq":1}}},{"$nor":[{"n_name":{"$eq":"VIETNAM"}},{"n_name":{"$eq":"UNITED KINGDOM"}}]}]},{}]}}]}},
{"$unwind":"$nation"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$nation"]}}},
{"$lookup":{"from":"supplier","localField":"n_nationkey","foreignField":"s_nationkey","as":"supplier","pipeline":[
{"$match":{"$and":[{"s_acctbal":{"$not":{"$eq":6399.78}}},{}]}}]}},
{"$unwind":"$supplier"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$supplier"]}}},
{"$lookup":{"from":"partsupp","localField":"s_suppkey","foreignField":"ps_suppkey","as":"partsupp","pipeline":[
{"$match":{"$and":[{"$and":[{"$or":[{"ps_availqty":{"$eq":2021}},{"ps_availqty":{"$gt":5420}},{"ps_availqty":{"$gte":3530}}]},{"$or":[{"ps_availqty":{"$eq":5432}},{"ps_supplycost":{"$gt":941.7}}]}]},{}]}}]}},
{"$unwind":"$partsupp"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$partsupp"]}}}]
));
```
Subjoin plan:
```
INLJ supplier.s_suppkey = ps_suppkey
  -> [none] INLJ nation_s.n_nationkey = s_nationkey
      -> [none] INLJ r_regionkey = n_regionkey
          -> [region_s] COLLSCAN: plan_stability_subjoin_cardinality_md.region {"$nor":[{"r_regionkey":{"$eq":0}},{"r_regionkey":{"$eq":3}}]} 
          -> [nation_s] FETCH: plan_stability_subjoin_cardinality_md.nation {"$and":[{"n_regionkey":{"$eq":4}},{"n_regionkey":{"$not":{"$eq":1}}},{"$nor":[{"n_name":{"$eq":"VIETNAM"}},{"n_name":{"$eq":"UNITED KINGDOM"}}]}]} 
              -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.nation n_regionkey_1
      -> [supplier] FETCH: plan_stability_subjoin_cardinality_md.supplier {"s_acctbal":{"$not":{"$eq":6399.78}}} 
          -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.supplier s_nationkey_1
  -> [partsupp] FETCH: plan_stability_subjoin_cardinality_md.partsupp {"$and":[{"$or":[{"ps_availqty":{"$eq":2021}},{"ps_availqty":{"$gt":5420}},{"ps_availqty":{"$gte":3530}}]},{"$or":[{"ps_availqty":{"$eq":5432}},{"ps_supplycost":{"$gt":941.7}}]}]} 
      -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.partsupp ps_suppkey_1
```
Estimated cardinality: 269  
Actual cardinality: 621  
Orders of magnitude: 0

---
### >>> Subjoin 5-4
```
db.region.aggregate(EJSON.deserialize(
[
{"$match":{"$nor":[{"r_regionkey":{"$eq":0}},{"r_regionkey":{"$eq":3}}]}},
{"$lookup":{"from":"nation","localField":"r_regionkey","foreignField":"n_regionkey","as":"nation","pipeline":[
{"$match":{"$and":[{"$and":[{"n_regionkey":{"$eq":4}},{"n_regionkey":{"$not":{"$eq":1}}},{"$nor":[{"n_name":{"$eq":"VIETNAM"}},{"n_name":{"$eq":"UNITED KINGDOM"}}]}]},{}]}}]}},
{"$unwind":"$nation"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$nation"]}}},
{"$lookup":{"from":"supplier","localField":"n_nationkey","foreignField":"s_nationkey","as":"supplier","pipeline":[
{"$match":{"$and":[{"s_acctbal":{"$not":{"$eq":6399.78}}},{}]}}]}},
{"$unwind":"$supplier"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$supplier"]}}},
{"$lookup":{"from":"partsupp","localField":"s_suppkey","foreignField":"ps_suppkey","as":"partsupp","pipeline":[
{"$match":{"$and":[{"$and":[{"$or":[{"ps_availqty":{"$eq":2021}},{"ps_availqty":{"$gt":5420}},{"ps_availqty":{"$gte":3530}}]},{"$or":[{"ps_availqty":{"$eq":5432}},{"ps_supplycost":{"$gt":941.7}}]}]},{}]}}]}},
{"$unwind":"$partsupp"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$partsupp"]}}},
{"$lookup":{"from":"part","localField":"ps_partkey","foreignField":"p_partkey","as":"part","pipeline":[
{"$match":{"p_name":{"$regex":"^for"}}}]}},
{"$unwind":"$part"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$part"]}}}]
));
```
Subjoin plan:
```
HJ p_partkey = partsupp.ps_partkey
  -> [none] COLLSCAN: plan_stability_subjoin_cardinality_md.part {"p_name":{"$regex":"^for"}} 
  -> [none] INLJ supplier.s_suppkey = ps_suppkey
      -> [none] INLJ nation_s.n_nationkey = s_nationkey
          -> [none] INLJ r_regionkey = n_regionkey
              -> [region_s] COLLSCAN: plan_stability_subjoin_cardinality_md.region {"$nor":[{"r_regionkey":{"$eq":0}},{"r_regionkey":{"$eq":3}}]} 
              -> [nation_s] FETCH: plan_stability_subjoin_cardinality_md.nation {"$and":[{"n_regionkey":{"$eq":4}},{"n_regionkey":{"$not":{"$eq":1}}},{"$nor":[{"n_name":{"$eq":"VIETNAM"}},{"n_name":{"$eq":"UNITED KINGDOM"}}]}]} 
                  -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.nation n_regionkey_1
          -> [supplier] FETCH: plan_stability_subjoin_cardinality_md.supplier {"s_acctbal":{"$not":{"$eq":6399.78}}} 
              -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.supplier s_nationkey_1
      -> [partsupp] FETCH: plan_stability_subjoin_cardinality_md.partsupp {"$and":[{"$or":[{"ps_availqty":{"$eq":2021}},{"ps_availqty":{"$gt":5420}},{"ps_availqty":{"$gte":3530}}]},{"$or":[{"ps_availqty":{"$eq":5432}},{"ps_supplycost":{"$gt":941.7}}]}]} 
          -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.partsupp ps_suppkey_1
```
Estimated cardinality: 2  
Actual cardinality: 7  
Orders of magnitude: 0

---
## >>> Command idx 6
```
{"aggregate":"supplier","pipeline":[
{"$lookup":{"from":"nation","localField":"s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$or":[{"n_name":{"$in":["UNITED STATES","IRAQ","UNITED KINGDOM"]}}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$lookup":{"from":"region","localField":"nation_s.n_regionkey","foreignField":"r_regionkey","pipeline":[
{"$match":{"$or":[{"r_regionkey":3},{"r_name":"EUROPE"}]}},
{"$match":{"$nor":[{"r_name":"AMERICA"},{"r_regionkey":1}]}}],"as":"region_s"}},
{"$unwind":"$region_s"},
{"$match":{"$nor":[{"s_acctbal":{"$lte":6089.75}}]}}],"cursor":{},"idx":6}
```
### >>> Subjoin 6-0
```
db.region.aggregate(EJSON.deserialize(
[
{"$match":{"$and":[{"$or":[{"r_name":{"$eq":"EUROPE"}},{"r_regionkey":{"$eq":3}}]},{"$nor":[{"r_name":{"$eq":"AMERICA"}},{"r_regionkey":{"$eq":1}}]}]}}]
));
```
Subjoin plan:
```
COLLSCAN: plan_stability_subjoin_cardinality_md.region {"$and":[{"$or":[{"r_name":{"$eq":"EUROPE"}},{"r_regionkey":{"$eq":3}}]},{"$nor":[{"r_name":{"$eq":"AMERICA"}},{"r_regionkey":{"$eq":1}}]}]}
```
Estimated cardinality: 1  
Actual cardinality: 1  
Orders of magnitude: 0

---
### >>> Subjoin 6-1
```
db.region.aggregate(EJSON.deserialize(
[
{"$match":{"$and":[{"$or":[{"r_name":{"$eq":"EUROPE"}},{"r_regionkey":{"$eq":3}}]},{"$nor":[{"r_name":{"$eq":"AMERICA"}},{"r_regionkey":{"$eq":1}}]}]}},
{"$lookup":{"from":"nation","localField":"r_regionkey","foreignField":"n_regionkey","as":"nation","pipeline":[
{"$match":{"n_name":{"$in":["IRAQ","UNITED KINGDOM","UNITED STATES"]}}}]}},
{"$unwind":"$nation"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$nation"]}}}]
));
```
Subjoin plan:
```
NLJ r_regionkey = n_regionkey
  -> [region_s] COLLSCAN: plan_stability_subjoin_cardinality_md.region {"$and":[{"$or":[{"r_name":{"$eq":"EUROPE"}},{"r_regionkey":{"$eq":3}}]},{"$nor":[{"r_name":{"$eq":"AMERICA"}},{"r_regionkey":{"$eq":1}}]}]} 
  -> [nation_s] COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"n_name":{"$in":["IRAQ","UNITED KINGDOM","UNITED STATES"]}}
```
Estimated cardinality: 1  
Actual cardinality: 1  
Orders of magnitude: 0

---
### >>> Subjoin 6-2
```
db.region.aggregate(EJSON.deserialize(
[
{"$match":{"$and":[{"$or":[{"r_name":{"$eq":"EUROPE"}},{"r_regionkey":{"$eq":3}}]},{"$nor":[{"r_name":{"$eq":"AMERICA"}},{"r_regionkey":{"$eq":1}}]}]}},
{"$lookup":{"from":"nation","localField":"r_regionkey","foreignField":"n_regionkey","as":"nation","pipeline":[
{"$match":{"n_name":{"$in":["IRAQ","UNITED KINGDOM","UNITED STATES"]}}}]}},
{"$unwind":"$nation"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$nation"]}}},
{"$lookup":{"from":"supplier","localField":"n_nationkey","foreignField":"s_nationkey","as":"supplier","pipeline":[
{"$match":{"$and":[{"s_acctbal":{"$not":{"$lte":6089.75}}},{}]}}]}},
{"$unwind":"$supplier"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$supplier"]}}}]
));
```
Subjoin plan:
```
INLJ nation_s.n_nationkey = s_nationkey
  -> [none] NLJ r_regionkey = n_regionkey
      -> [region_s] COLLSCAN: plan_stability_subjoin_cardinality_md.region {"$and":[{"$or":[{"r_name":{"$eq":"EUROPE"}},{"r_regionkey":{"$eq":3}}]},{"$nor":[{"r_name":{"$eq":"AMERICA"}},{"r_regionkey":{"$eq":1}}]}]} 
      -> [nation_s] COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"n_name":{"$in":["IRAQ","UNITED KINGDOM","UNITED STATES"]}} 
  -> [none] FETCH: plan_stability_subjoin_cardinality_md.supplier {"s_acctbal":{"$not":{"$lte":6089.75}}} 
      -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.supplier s_nationkey_1
```
Estimated cardinality: 9  
Actual cardinality: 13  
Orders of magnitude: 1

---
## >>> Command idx 8
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$or":[{"ps_availqty":{"$lt":6696}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"supplier","localField":"partsupp.ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$nor":[{"s_name":"Supplier#000000335"},{"s_acctbal":{"$eq":8724.42}}]}},
{"$match":{"$nor":[{"s_acctbal":{"$gt":958.07}}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$or":[{"n_name":{"$nin":["SAUDI ARABIA","ARGENTINA","MOZAMBIQUE"]}}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$lookup":{"from":"region","localField":"nation_s.n_regionkey","foreignField":"r_regionkey","pipeline":[
{"$match":{"$and":[{"r_name":"MIDDLE EAST"}]}},
{"$match":{"$nor":[{"r_name":"AMERICA"},{"r_regionkey":0}]}}],"as":"region_s"}},
{"$unwind":"$region_s"},
{"$match":{"$and":[{"p_type":{"$regex":{"$regex":"^ECONOMY","$options":""}}}]}}],"cursor":{},"idx":8}
```
### >>> Subjoin 8-0
```
db.region.aggregate(EJSON.deserialize(
[
{"$match":{"$and":[{"r_name":{"$eq":"MIDDLE EAST"}},{"$nor":[{"r_name":{"$eq":"AMERICA"}},{"r_regionkey":{"$eq":0}}]}]}}]
));
```
Subjoin plan:
```
COLLSCAN: plan_stability_subjoin_cardinality_md.region {"$and":[{"r_name":{"$eq":"MIDDLE EAST"}},{"$nor":[{"r_name":{"$eq":"AMERICA"}},{"r_regionkey":{"$eq":0}}]}]}
```
Estimated cardinality: 1  
Actual cardinality: 1  
Orders of magnitude: 0

---
### >>> Subjoin 8-1
```
db.region.aggregate(EJSON.deserialize(
[
{"$match":{"$and":[{"r_name":{"$eq":"MIDDLE EAST"}},{"$nor":[{"r_name":{"$eq":"AMERICA"}},{"r_regionkey":{"$eq":0}}]}]}},
{"$lookup":{"from":"nation","localField":"r_regionkey","foreignField":"n_regionkey","as":"nation","pipeline":[
{"$match":{"n_name":{"$not":{"$in":["ARGENTINA","MOZAMBIQUE","SAUDI ARABIA"]}}}}]}},
{"$unwind":"$nation"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$nation"]}}}]
));
```
Subjoin plan:
```
NLJ r_regionkey = n_regionkey
  -> [region_s] COLLSCAN: plan_stability_subjoin_cardinality_md.region {"$and":[{"r_name":{"$eq":"MIDDLE EAST"}},{"$nor":[{"r_name":{"$eq":"AMERICA"}},{"r_regionkey":{"$eq":0}}]}]} 
  -> [nation_s] COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"n_name":{"$not":{"$in":["ARGENTINA","MOZAMBIQUE","SAUDI ARABIA"]}}}
```
Estimated cardinality: 4  
Actual cardinality: 4  
Orders of magnitude: 0

---
### >>> Subjoin 8-2
```
db.region.aggregate(EJSON.deserialize(
[
{"$match":{"$and":[{"r_name":{"$eq":"MIDDLE EAST"}},{"$nor":[{"r_name":{"$eq":"AMERICA"}},{"r_regionkey":{"$eq":0}}]}]}},
{"$lookup":{"from":"nation","localField":"r_regionkey","foreignField":"n_regionkey","as":"nation","pipeline":[
{"$match":{"n_name":{"$not":{"$in":["ARGENTINA","MOZAMBIQUE","SAUDI ARABIA"]}}}}]}},
{"$unwind":"$nation"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$nation"]}}},
{"$lookup":{"from":"supplier","localField":"n_nationkey","foreignField":"s_nationkey","as":"supplier","pipeline":[
{"$match":{"$and":[{"s_acctbal":{"$not":{"$gt":958.07}}},{"$nor":[{"s_acctbal":{"$eq":8724.42}},{"s_name":{"$eq":"Supplier#000000335"}}]}]}}]}},
{"$unwind":"$supplier"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$supplier"]}}}]
));
```
Subjoin plan:
```
HJ nation_s.n_nationkey = s_nationkey
  -> [none] NLJ r_regionkey = n_regionkey
      -> [region_s] COLLSCAN: plan_stability_subjoin_cardinality_md.region {"$and":[{"r_name":{"$eq":"MIDDLE EAST"}},{"$nor":[{"r_name":{"$eq":"AMERICA"}},{"r_regionkey":{"$eq":0}}]}]} 
      -> [nation_s] COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"n_name":{"$not":{"$in":["ARGENTINA","MOZAMBIQUE","SAUDI ARABIA"]}}} 
  -> [supplier] COLLSCAN: plan_stability_subjoin_cardinality_md.supplier {"$and":[{"s_acctbal":{"$not":{"$gt":958.07}}},{"$nor":[{"s_acctbal":{"$eq":8724.42}},{"s_name":{"$eq":"Supplier#000000335"}}]}]}
```
Estimated cardinality: 32  
Actual cardinality: 23  
Orders of magnitude: 0

---
### >>> Subjoin 8-3
```
db.region.aggregate(EJSON.deserialize(
[
{"$match":{"$and":[{"r_name":{"$eq":"MIDDLE EAST"}},{"$nor":[{"r_name":{"$eq":"AMERICA"}},{"r_regionkey":{"$eq":0}}]}]}},
{"$lookup":{"from":"nation","localField":"r_regionkey","foreignField":"n_regionkey","as":"nation","pipeline":[
{"$match":{"n_name":{"$not":{"$in":["ARGENTINA","MOZAMBIQUE","SAUDI ARABIA"]}}}}]}},
{"$unwind":"$nation"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$nation"]}}},
{"$lookup":{"from":"supplier","localField":"n_nationkey","foreignField":"s_nationkey","as":"supplier","pipeline":[
{"$match":{"$and":[{"s_acctbal":{"$not":{"$gt":958.07}}},{"$nor":[{"s_acctbal":{"$eq":8724.42}},{"s_name":{"$eq":"Supplier#000000335"}}]}]}}]}},
{"$unwind":"$supplier"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$supplier"]}}},
{"$lookup":{"from":"partsupp","localField":"s_suppkey","foreignField":"ps_suppkey","as":"partsupp","pipeline":[
{"$match":{"$and":[{"ps_availqty":{"$lt":6696}},{}]}}]}},
{"$unwind":"$partsupp"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$partsupp"]}}}]
));
```
Subjoin plan:
```
INLJ supplier.s_suppkey = ps_suppkey
  -> [none] HJ nation_s.n_nationkey = s_nationkey
      -> [none] NLJ r_regionkey = n_regionkey
          -> [region_s] COLLSCAN: plan_stability_subjoin_cardinality_md.region {"$and":[{"r_name":{"$eq":"MIDDLE EAST"}},{"$nor":[{"r_name":{"$eq":"AMERICA"}},{"r_regionkey":{"$eq":0}}]}]} 
          -> [nation_s] COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"n_name":{"$not":{"$in":["ARGENTINA","MOZAMBIQUE","SAUDI ARABIA"]}}} 
      -> [supplier] COLLSCAN: plan_stability_subjoin_cardinality_md.supplier {"$and":[{"s_acctbal":{"$not":{"$gt":958.07}}},{"$nor":[{"s_acctbal":{"$eq":8724.42}},{"s_name":{"$eq":"Supplier#000000335"}}]}]} 
  -> [partsupp] FETCH: plan_stability_subjoin_cardinality_md.partsupp {"ps_availqty":{"$lt":6696}} 
      -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.partsupp ps_suppkey_1
```
Estimated cardinality: 1718  
Actual cardinality: 1245  
Orders of magnitude: 0

---
### >>> Subjoin 8-4
```
db.region.aggregate(EJSON.deserialize(
[
{"$match":{"$and":[{"r_name":{"$eq":"MIDDLE EAST"}},{"$nor":[{"r_name":{"$eq":"AMERICA"}},{"r_regionkey":{"$eq":0}}]}]}},
{"$lookup":{"from":"nation","localField":"r_regionkey","foreignField":"n_regionkey","as":"nation","pipeline":[
{"$match":{"n_name":{"$not":{"$in":["ARGENTINA","MOZAMBIQUE","SAUDI ARABIA"]}}}}]}},
{"$unwind":"$nation"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$nation"]}}},
{"$lookup":{"from":"supplier","localField":"n_nationkey","foreignField":"s_nationkey","as":"supplier","pipeline":[
{"$match":{"$and":[{"s_acctbal":{"$not":{"$gt":958.07}}},{"$nor":[{"s_acctbal":{"$eq":8724.42}},{"s_name":{"$eq":"Supplier#000000335"}}]}]}}]}},
{"$unwind":"$supplier"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$supplier"]}}},
{"$lookup":{"from":"partsupp","localField":"s_suppkey","foreignField":"ps_suppkey","as":"partsupp","pipeline":[
{"$match":{"$and":[{"ps_availqty":{"$lt":6696}},{}]}}]}},
{"$unwind":"$partsupp"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$partsupp"]}}},
{"$lookup":{"from":"part","localField":"ps_partkey","foreignField":"p_partkey","as":"part","pipeline":[
{"$match":{"p_type":{"$regex":"^ECONOMY"}}}]}},
{"$unwind":"$part"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$part"]}}}]
));
```
Subjoin plan:
```
HJ partsupp.ps_partkey = p_partkey
  -> [none] INLJ supplier.s_suppkey = ps_suppkey
      -> [none] HJ nation_s.n_nationkey = s_nationkey
          -> [none] NLJ r_regionkey = n_regionkey
              -> [region_s] COLLSCAN: plan_stability_subjoin_cardinality_md.region {"$and":[{"r_name":{"$eq":"MIDDLE EAST"}},{"$nor":[{"r_name":{"$eq":"AMERICA"}},{"r_regionkey":{"$eq":0}}]}]} 
              -> [nation_s] COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"n_name":{"$not":{"$in":["ARGENTINA","MOZAMBIQUE","SAUDI ARABIA"]}}} 
          -> [supplier] COLLSCAN: plan_stability_subjoin_cardinality_md.supplier {"$and":[{"s_acctbal":{"$not":{"$gt":958.07}}},{"$nor":[{"s_acctbal":{"$eq":8724.42}},{"s_name":{"$eq":"Supplier#000000335"}}]}]} 
      -> [partsupp] FETCH: plan_stability_subjoin_cardinality_md.partsupp {"ps_availqty":{"$lt":6696}} 
          -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.partsupp ps_suppkey_1
  -> [none] COLLSCAN: plan_stability_subjoin_cardinality_md.part {"p_type":{"$regex":"^ECONOMY"}}
```
Estimated cardinality: 249  
Actual cardinality: 193  
Orders of magnitude: 0

---
## >>> Command idx 9
```
{"aggregate":"supplier","pipeline":[
{"$lookup":{"from":"nation","localField":"s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$and":[{"n_name":{"$nin":["MOROCCO","MOROCCO","CHINA"]}}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$lookup":{"from":"region","localField":"nation_s.n_regionkey","foreignField":"r_regionkey","pipeline":[
{"$match":{"$nor":[{"r_regionkey":4},{"r_name":"MIDDLE EAST"}]}},
{"$match":{"$nor":[{"r_name":"MIDDLE EAST"}]}}],"as":"region_s"}},
{"$unwind":"$region_s"},
{"$match":{"$nor":[{"s_name":"Supplier#000000785"}]}}],"cursor":{},"idx":9}
```
### >>> Subjoin 9-0
```
db.region.aggregate(EJSON.deserialize(
[
{"$match":{"$and":[{"r_name":{"$not":{"$eq":"MIDDLE EAST"}}},{"$nor":[{"r_name":{"$eq":"MIDDLE EAST"}},{"r_regionkey":{"$eq":4}}]}]}}]
));
```
Subjoin plan:
```
COLLSCAN: plan_stability_subjoin_cardinality_md.region {"$and":[{"r_name":{"$not":{"$eq":"MIDDLE EAST"}}},{"$nor":[{"r_name":{"$eq":"MIDDLE EAST"}},{"r_regionkey":{"$eq":4}}]}]}
```
Estimated cardinality: 4  
Actual cardinality: 4  
Orders of magnitude: 0

---
### >>> Subjoin 9-1
```
db.region.aggregate(EJSON.deserialize(
[
{"$match":{"$and":[{"r_name":{"$not":{"$eq":"MIDDLE EAST"}}},{"$nor":[{"r_name":{"$eq":"MIDDLE EAST"}},{"r_regionkey":{"$eq":4}}]}]}},
{"$lookup":{"from":"nation","localField":"r_regionkey","foreignField":"n_regionkey","as":"nation","pipeline":[
{"$match":{"n_name":{"$not":{"$in":["CHINA","MOROCCO"]}}}}]}},
{"$unwind":"$nation"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$nation"]}}}]
));
```
Subjoin plan:
```
HJ r_regionkey = n_regionkey
  -> [region_s] COLLSCAN: plan_stability_subjoin_cardinality_md.region {"$and":[{"r_name":{"$not":{"$eq":"MIDDLE EAST"}}},{"$nor":[{"r_name":{"$eq":"MIDDLE EAST"}},{"r_regionkey":{"$eq":4}}]}]} 
  -> [nation_s] COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"n_name":{"$not":{"$in":["CHINA","MOROCCO"]}}}
```
Estimated cardinality: 18  
Actual cardinality: 18  
Orders of magnitude: 0

---
### >>> Subjoin 9-2
```
db.region.aggregate(EJSON.deserialize(
[
{"$match":{"$and":[{"r_name":{"$not":{"$eq":"MIDDLE EAST"}}},{"$nor":[{"r_name":{"$eq":"MIDDLE EAST"}},{"r_regionkey":{"$eq":4}}]}]}},
{"$lookup":{"from":"nation","localField":"r_regionkey","foreignField":"n_regionkey","as":"nation","pipeline":[
{"$match":{"n_name":{"$not":{"$in":["CHINA","MOROCCO"]}}}}]}},
{"$unwind":"$nation"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$nation"]}}},
{"$lookup":{"from":"supplier","localField":"n_nationkey","foreignField":"s_nationkey","as":"supplier","pipeline":[
{"$match":{"s_name":{"$not":{"$eq":"Supplier#000000785"}}}}]}},
{"$unwind":"$supplier"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$supplier"]}}}]
));
```
Subjoin plan:
```
HJ nation_s.n_nationkey = s_nationkey
  -> [none] HJ r_regionkey = n_regionkey
      -> [region_s] COLLSCAN: plan_stability_subjoin_cardinality_md.region {"$and":[{"r_name":{"$not":{"$eq":"MIDDLE EAST"}}},{"$nor":[{"r_name":{"$eq":"MIDDLE EAST"}},{"r_regionkey":{"$eq":4}}]}]} 
      -> [nation_s] COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"n_name":{"$not":{"$in":["CHINA","MOROCCO"]}}} 
  -> [none] COLLSCAN: plan_stability_subjoin_cardinality_md.supplier {"s_name":{"$not":{"$eq":"Supplier#000000785"}}}
```
Estimated cardinality: 735  
Actual cardinality: 708  
Orders of magnitude: 0

---
## >>> Command idx 10
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$and":[{"ps_availqty":{"$eq":1414}}]}},
{"$match":{"$nor":[{"ps_comment":{"$regex":{"$regex":"^usl","$options":""}}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"supplier","localField":"partsupp.ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$nor":[{"s_name":"Supplier#000000137"},{"s_nationkey":20}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$or":[{"n_regionkey":0},{"n_name":{"$in":["IRAQ","SAUDI ARABIA","PERU"]}}]}},
{"$match":{"$and":[{"n_name":{"$nin":["ETHIOPIA","MOZAMBIQUE"]}}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$match":{"$nor":[{"supplier.s_name":"Supplier#000000148"},{"p_retailprice":{"$lte":1699.79}}]}}],"cursor":{},"idx":10}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 11
```
{"aggregate":"lineitem","pipeline":[
{"$lookup":{"from":"orders","localField":"l_orderkey","foreignField":"o_orderkey","pipeline":[
{"$match":{"$and":[{"o_orderdate":{"$gt":"1998-02-20T00:00:00.000Z"}}]}},
{"$match":{"$and":[{"o_orderpriority":"3-MEDIUM"}]}}],"as":"orders"}},
{"$unwind":"$orders"},
{"$lookup":{"from":"customer","localField":"orders.o_custkey","foreignField":"c_custkey","pipeline":[
{"$match":{"$and":[{"c_acctbal":{"$gte":6089.13}}]}}],"as":"customer"}},
{"$unwind":"$customer"},
{"$lookup":{"from":"supplier","localField":"customer.c_nationkey","foreignField":"s_nationkey","pipeline":[
{"$match":{"$or":[{"s_acctbal":{"$eq":3580.35}},{"s_name":"Supplier#000000338"}]}},
{"$match":{"$nor":[{"s_nationkey":13},{"s_nationkey":6}]}},
{"$match":{"$nor":[{"s_acctbal":{"$eq":3839.44}},{"s_acctbal":{"$gt":9583.11}}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$match":{"$and":[{"customer.c_mktsegment":"MACHINERY"}]}}],"cursor":{},"idx":11}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 12
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$and":[{"ps_availqty":{"$gt":6300}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"supplier","localField":"partsupp.ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$or":[{"s_nationkey":9}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$nor":[{"n_name":{"$in":["KENYA","CHINA"]}}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$lookup":{"from":"region","localField":"nation_s.n_regionkey","foreignField":"r_regionkey","pipeline":[
{"$match":{"$or":[{"r_regionkey":2},{"r_name":"AMERICA"}]}}],"as":"region_s"}},
{"$unwind":"$region_s"},
{"$match":{"$and":[{"p_name":{"$regex":{"$regex":"^d","$options":""}}}]}}],"cursor":{},"idx":12}
```
### >>> Subjoin 12-0
```
db.region.aggregate(EJSON.deserialize(
[
{"$match":{"$or":[{"r_name":{"$eq":"AMERICA"}},{"r_regionkey":{"$eq":2}}]}}]
));
```
Subjoin plan:
```
COLLSCAN: plan_stability_subjoin_cardinality_md.region {"$or":[{"r_name":{"$eq":"AMERICA"}},{"r_regionkey":{"$eq":2}}]}
```
Estimated cardinality: 2  
Actual cardinality: 2  
Orders of magnitude: 0

---
### >>> Subjoin 12-1
```
db.region.aggregate(EJSON.deserialize(
[
{"$match":{"$or":[{"r_name":{"$eq":"AMERICA"}},{"r_regionkey":{"$eq":2}}]}},
{"$lookup":{"from":"nation","localField":"r_regionkey","foreignField":"n_regionkey","as":"nation","pipeline":[
{"$match":{"n_name":{"$not":{"$in":["CHINA","KENYA"]}}}}]}},
{"$unwind":"$nation"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$nation"]}}}]
));
```
Subjoin plan:
```
HJ r_regionkey = n_regionkey
  -> [region_s] COLLSCAN: plan_stability_subjoin_cardinality_md.region {"$or":[{"r_name":{"$eq":"AMERICA"}},{"r_regionkey":{"$eq":2}}]} 
  -> [nation_s] COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"n_name":{"$not":{"$in":["CHINA","KENYA"]}}}
```
Estimated cardinality: 9  
Actual cardinality: 9  
Orders of magnitude: 0

---
### >>> Subjoin 12-2
```
db.region.aggregate(EJSON.deserialize(
[
{"$match":{"$or":[{"r_name":{"$eq":"AMERICA"}},{"r_regionkey":{"$eq":2}}]}},
{"$lookup":{"from":"nation","localField":"r_regionkey","foreignField":"n_regionkey","as":"nation","pipeline":[
{"$match":{"n_name":{"$not":{"$in":["CHINA","KENYA"]}}}}]}},
{"$unwind":"$nation"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$nation"]}}},
{"$lookup":{"from":"supplier","localField":"n_nationkey","foreignField":"s_nationkey","as":"supplier","pipeline":[
{"$match":{"s_nationkey":9}}]}},
{"$unwind":"$supplier"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$supplier"]}}}]
));
```
Subjoin plan:
```
HJ nation_s.n_nationkey = s_nationkey
  -> [none] HJ r_regionkey = n_regionkey
      -> [region_s] COLLSCAN: plan_stability_subjoin_cardinality_md.region {"$or":[{"r_name":{"$eq":"AMERICA"}},{"r_regionkey":{"$eq":2}}]} 
      -> [nation_s] COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"n_name":{"$not":{"$in":["CHINA","KENYA"]}}} 
  -> [supplier] FETCH: plan_stability_subjoin_cardinality_md.supplier 
      -> IXSCAN: plan_stability_subjoin_cardinality_md.supplier s_nationkey_1 {"s_nationkey":["[9.0, 9.0]"]}
```
Estimated cardinality: 17  
Actual cardinality: 45  
Orders of magnitude: 0

---
### >>> Subjoin 12-3
```
db.region.aggregate(EJSON.deserialize(
[
{"$match":{"$or":[{"r_name":{"$eq":"AMERICA"}},{"r_regionkey":{"$eq":2}}]}},
{"$lookup":{"from":"nation","localField":"r_regionkey","foreignField":"n_regionkey","as":"nation","pipeline":[
{"$match":{"n_name":{"$not":{"$in":["CHINA","KENYA"]}}}}]}},
{"$unwind":"$nation"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$nation"]}}},
{"$lookup":{"from":"supplier","localField":"n_nationkey","foreignField":"s_nationkey","as":"supplier","pipeline":[
{"$match":{"s_nationkey":9}}]}},
{"$unwind":"$supplier"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$supplier"]}}},
{"$lookup":{"from":"partsupp","localField":"s_suppkey","foreignField":"ps_suppkey","as":"partsupp","pipeline":[
{"$match":{"$and":[{"ps_availqty":{"$gt":6300}},{}]}}]}},
{"$unwind":"$partsupp"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$partsupp"]}}}]
));
```
Subjoin plan:
```
INLJ supplier.s_suppkey = ps_suppkey
  -> [none] HJ nation_s.n_nationkey = s_nationkey
      -> [none] HJ r_regionkey = n_regionkey
          -> [region_s] COLLSCAN: plan_stability_subjoin_cardinality_md.region {"$or":[{"r_name":{"$eq":"AMERICA"}},{"r_regionkey":{"$eq":2}}]} 
          -> [nation_s] COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"n_name":{"$not":{"$in":["CHINA","KENYA"]}}} 
      -> [supplier] FETCH: plan_stability_subjoin_cardinality_md.supplier 
          -> IXSCAN: plan_stability_subjoin_cardinality_md.supplier s_nationkey_1 {"s_nationkey":["[9.0, 9.0]"]}
  -> [partsupp] FETCH: plan_stability_subjoin_cardinality_md.partsupp {"ps_availqty":{"$gt":6300}} 
      -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.partsupp ps_suppkey_1
```
Estimated cardinality: 484  
Actual cardinality: 1333  
Orders of magnitude: 1

---
### >>> Subjoin 12-4
```
db.region.aggregate(EJSON.deserialize(
[
{"$match":{"$or":[{"r_name":{"$eq":"AMERICA"}},{"r_regionkey":{"$eq":2}}]}},
{"$lookup":{"from":"nation","localField":"r_regionkey","foreignField":"n_regionkey","as":"nation","pipeline":[
{"$match":{"n_name":{"$not":{"$in":["CHINA","KENYA"]}}}}]}},
{"$unwind":"$nation"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$nation"]}}},
{"$lookup":{"from":"supplier","localField":"n_nationkey","foreignField":"s_nationkey","as":"supplier","pipeline":[
{"$match":{"s_nationkey":9}}]}},
{"$unwind":"$supplier"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$supplier"]}}},
{"$lookup":{"from":"partsupp","localField":"s_suppkey","foreignField":"ps_suppkey","as":"partsupp","pipeline":[
{"$match":{"$and":[{"ps_availqty":{"$gt":6300}},{}]}}]}},
{"$unwind":"$partsupp"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$partsupp"]}}},
{"$lookup":{"from":"part","localField":"ps_partkey","foreignField":"p_partkey","as":"part","pipeline":[
{"$match":{"p_name":{"$regex":"^d"}}}]}},
{"$unwind":"$part"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$part"]}}}]
));
```
Subjoin plan:
```
HJ partsupp.ps_partkey = p_partkey
  -> [none] INLJ supplier.s_suppkey = ps_suppkey
      -> [none] HJ nation_s.n_nationkey = s_nationkey
          -> [none] HJ r_regionkey = n_regionkey
              -> [region_s] COLLSCAN: plan_stability_subjoin_cardinality_md.region {"$or":[{"r_name":{"$eq":"AMERICA"}},{"r_regionkey":{"$eq":2}}]} 
              -> [nation_s] COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"n_name":{"$not":{"$in":["CHINA","KENYA"]}}} 
          -> [supplier] FETCH: plan_stability_subjoin_cardinality_md.supplier 
              -> IXSCAN: plan_stability_subjoin_cardinality_md.supplier s_nationkey_1 {"s_nationkey":["[9.0, 9.0]"]}
      -> [partsupp] FETCH: plan_stability_subjoin_cardinality_md.partsupp {"ps_availqty":{"$gt":6300}} 
          -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.partsupp ps_suppkey_1
  -> [none] COLLSCAN: plan_stability_subjoin_cardinality_md.part {"p_name":{"$regex":"^d"}}
```
Estimated cardinality: 28  
Actual cardinality: 66  
Orders of magnitude: 0

---
## >>> Command idx 13
```
{"aggregate":"supplier","pipeline":[
{"$lookup":{"from":"nation","localField":"s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$nor":[{"n_name":"MOZAMBIQUE"}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$lookup":{"from":"region","localField":"nation_s.n_regionkey","foreignField":"r_regionkey","pipeline":[
{"$match":{"$and":[{"r_name":"AMERICA"},{"r_regionkey":1}]}},
{"$match":{"$nor":[{"r_name":"AFRICA"},{"r_name":"MIDDLE EAST"},{"r_name":"EUROPE"},{"r_name":"EUROPE"}]}}],"as":"region_s"}},
{"$unwind":"$region_s"},
{"$match":{"$or":[{"nation_s.n_regionkey":4},{"nation_s.n_regionkey":1},{"region_s.r_name":"ASIA"}]}}],"cursor":{},"idx":13}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 14
```
{"aggregate":"supplier","pipeline":[
{"$lookup":{"from":"nation","localField":"s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$nor":[{"n_regionkey":3}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$lookup":{"from":"region","localField":"nation_s.n_regionkey","foreignField":"r_regionkey","pipeline":[
{"$match":{"$nor":[{"r_regionkey":0}]}}],"as":"region_s"}},
{"$unwind":"$region_s"},
{"$match":{"$nor":[{"region_s.r_regionkey":2}]}}],"cursor":{},"idx":14}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 15
```
{"aggregate":"lineitem","pipeline":[
{"$lookup":{"from":"orders","localField":"l_orderkey","foreignField":"o_orderkey","pipeline":[
{"$match":{"$nor":[{"o_totalprice":{"$gt":6549.4}},{"o_orderpriority":"5-LOW"},{"o_orderpriority":"3-MEDIUM"}]}},
{"$match":{"$nor":[{"o_orderdate":{"$gt":"1996-01-31T00:00:00.000Z"}}]}},
{"$match":{"$nor":[{"o_clerk":"Clerk#000000052"},{"o_shippriority":{"$gt":0}},{"o_orderstatus":"P"}]}}],"as":"orders"}},
{"$unwind":"$orders"},
{"$lookup":{"from":"customer","localField":"orders.o_custkey","foreignField":"c_custkey","pipeline":[
{"$match":{"$and":[{"c_acctbal":{"$gt":1687.58}}]}},
{"$match":{"$nor":[{"c_mktsegment":"MACHINERY"}]}}],"as":"customer"}},
{"$unwind":"$customer"},
{"$lookup":{"from":"supplier","localField":"customer.c_nationkey","foreignField":"s_nationkey","pipeline":[
{"$match":{"$or":[{"s_nationkey":13}]}},
{"$match":{"$nor":[{"s_acctbal":{"$gte":-707.02}},{"s_name":"Supplier#000000717"}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$match":{"$nor":[{"l_receiptdate":{"$gte":"1996-12-14T00:00:00.000Z"}}]}}],"cursor":{},"idx":15}
```
### >>> Subjoin 15-0
```
db.supplier.aggregate(EJSON.deserialize(
[
{"$match":{"$and":[{"$nor":[{"s_acctbal":{"$gte":-707.02}},{"s_name":{"$eq":"Supplier#000000717"}}]},{"s_nationkey":13}]}}]
));
```
Subjoin plan:
```
FETCH: plan_stability_subjoin_cardinality_md.supplier {"$nor":[{"s_acctbal":{"$gte":-707.02}},{"s_name":{"$eq":"Supplier#000000717"}}]} 
  -> IXSCAN: plan_stability_subjoin_cardinality_md.supplier s_nationkey_1 {"s_nationkey":["[13.0, 13.0]"]}
```
Estimated cardinality: 1  
Actual cardinality: 1  
Orders of magnitude: 0

---
### >>> Subjoin 15-1
```
db.supplier.aggregate(EJSON.deserialize(
[
{"$match":{"$and":[{"$nor":[{"s_acctbal":{"$gte":-707.02}},{"s_name":{"$eq":"Supplier#000000717"}}]},{"s_nationkey":13}]}},
{"$lookup":{"from":"customer","localField":"s_nationkey","foreignField":"c_nationkey","as":"customer","pipeline":[
{"$match":{"$and":[{"$and":[{"c_acctbal":{"$gt":1687.58}},{"c_mktsegment":{"$not":{"$eq":"MACHINERY"}}}]},{}]}}]}},
{"$unwind":"$customer"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$customer"]}}}]
));
```
Subjoin plan:
```
INLJ s_nationkey = c_nationkey
  -> [supplier] FETCH: plan_stability_subjoin_cardinality_md.supplier {"$nor":[{"s_acctbal":{"$gte":-707.02}},{"s_name":{"$eq":"Supplier#000000717"}}]} 
      -> IXSCAN: plan_stability_subjoin_cardinality_md.supplier s_nationkey_1 {"s_nationkey":["[13.0, 13.0]"]}
  -> [customer] FETCH: plan_stability_subjoin_cardinality_md.customer {"$and":[{"c_acctbal":{"$gt":1687.58}},{"c_mktsegment":{"$not":{"$eq":"MACHINERY"}}}]} 
      -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.customer c_nationkey_1
```
Estimated cardinality: 375  
Actual cardinality: 355  
Orders of magnitude: 0

---
### >>> Subjoin 15-2
```
db.supplier.aggregate(EJSON.deserialize(
[
{"$match":{"$and":[{"$nor":[{"s_acctbal":{"$gte":-707.02}},{"s_name":{"$eq":"Supplier#000000717"}}]},{"s_nationkey":13}]}},
{"$lookup":{"from":"customer","localField":"s_nationkey","foreignField":"c_nationkey","as":"customer","pipeline":[
{"$match":{"$and":[{"$and":[{"c_acctbal":{"$gt":1687.58}},{"c_mktsegment":{"$not":{"$eq":"MACHINERY"}}}]},{}]}}]}},
{"$unwind":"$customer"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$customer"]}}},
{"$lookup":{"from":"orders","localField":"c_custkey","foreignField":"o_custkey","as":"orders","pipeline":[
{"$match":{"$and":[{"o_orderdate":{"$not":{"$gt":"1996-01-31T00:00:00.000Z"}}},{"$nor":[{"o_clerk":{"$eq":"Clerk#000000052"}},{"o_orderstatus":{"$eq":"P"}},{"o_shippriority":{"$gt":0}}]},{"$nor":[{"o_orderpriority":{"$eq":"5-LOW"}},{"o_orderpriority":{"$eq":"3-MEDIUM"}},{"o_totalprice":{"$gt":6549.4}}]}]}}]}},
{"$unwind":"$orders"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$orders"]}}}]
));
```
Subjoin plan:
```
HJ customer.c_custkey = o_custkey
  -> [none] INLJ s_nationkey = c_nationkey
      -> [supplier] FETCH: plan_stability_subjoin_cardinality_md.supplier {"$nor":[{"s_acctbal":{"$gte":-707.02}},{"s_name":{"$eq":"Supplier#000000717"}}]} 
          -> IXSCAN: plan_stability_subjoin_cardinality_md.supplier s_nationkey_1 {"s_nationkey":["[13.0, 13.0]"]}
      -> [customer] FETCH: plan_stability_subjoin_cardinality_md.customer {"$and":[{"c_acctbal":{"$gt":1687.58}},{"c_mktsegment":{"$not":{"$eq":"MACHINERY"}}}]} 
          -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.customer c_nationkey_1
  -> [orders] COLLSCAN: plan_stability_subjoin_cardinality_md.orders {"$and":[{"o_orderdate":{"$not":{"$gt":"1996-01-31T00:00:00.000Z"}}},{"$nor":[{"o_clerk":{"$eq":"Clerk#000000052"}},{"o_orderstatus":{"$eq":"P"}},{"o_shippriority":{"$gt":0}}]},{"$nor":[{"o_orderpriority":{"$eq":"5-LOW"}},{"o_orderpriority":{"$eq":"3-MEDIUM"}},{"o_totalprice":{"$gt":6549.4}}]}]}
```
Estimated cardinality: 15  
Actual cardinality: 23  
Orders of magnitude: 0

---
### >>> Subjoin 15-3
```
db.supplier.aggregate(EJSON.deserialize(
[
{"$match":{"$and":[{"$nor":[{"s_acctbal":{"$gte":-707.02}},{"s_name":{"$eq":"Supplier#000000717"}}]},{"s_nationkey":13}]}},
{"$lookup":{"from":"customer","localField":"s_nationkey","foreignField":"c_nationkey","as":"customer","pipeline":[
{"$match":{"$and":[{"$and":[{"c_acctbal":{"$gt":1687.58}},{"c_mktsegment":{"$not":{"$eq":"MACHINERY"}}}]},{}]}}]}},
{"$unwind":"$customer"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$customer"]}}},
{"$lookup":{"from":"orders","localField":"c_custkey","foreignField":"o_custkey","as":"orders","pipeline":[
{"$match":{"$and":[{"o_orderdate":{"$not":{"$gt":"1996-01-31T00:00:00.000Z"}}},{"$nor":[{"o_clerk":{"$eq":"Clerk#000000052"}},{"o_orderstatus":{"$eq":"P"}},{"o_shippriority":{"$gt":0}}]},{"$nor":[{"o_orderpriority":{"$eq":"5-LOW"}},{"o_orderpriority":{"$eq":"3-MEDIUM"}},{"o_totalprice":{"$gt":6549.4}}]}]}}]}},
{"$unwind":"$orders"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$orders"]}}},
{"$lookup":{"from":"lineitem","localField":"o_orderkey","foreignField":"l_orderkey","as":"lineitem","pipeline":[
{"$match":{"$and":[{"l_receiptdate":{"$not":{"$gte":"1996-12-14T00:00:00.000Z"}}},{}]}}]}},
{"$unwind":"$lineitem"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$lineitem"]}}}]
));
```
Subjoin plan:
```
INLJ orders.o_orderkey = l_orderkey
  -> [none] HJ customer.c_custkey = o_custkey
      -> [none] INLJ s_nationkey = c_nationkey
          -> [supplier] FETCH: plan_stability_subjoin_cardinality_md.supplier {"$nor":[{"s_acctbal":{"$gte":-707.02}},{"s_name":{"$eq":"Supplier#000000717"}}]} 
              -> IXSCAN: plan_stability_subjoin_cardinality_md.supplier s_nationkey_1 {"s_nationkey":["[13.0, 13.0]"]}
          -> [customer] FETCH: plan_stability_subjoin_cardinality_md.customer {"$and":[{"c_acctbal":{"$gt":1687.58}},{"c_mktsegment":{"$not":{"$eq":"MACHINERY"}}}]} 
              -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.customer c_nationkey_1
      -> [orders] COLLSCAN: plan_stability_subjoin_cardinality_md.orders {"$and":[{"o_orderdate":{"$not":{"$gt":"1996-01-31T00:00:00.000Z"}}},{"$nor":[{"o_clerk":{"$eq":"Clerk#000000052"}},{"o_orderstatus":{"$eq":"P"}},{"o_shippriority":{"$gt":0}}]},{"$nor":[{"o_orderpriority":{"$eq":"5-LOW"}},{"o_orderpriority":{"$eq":"3-MEDIUM"}},{"o_totalprice":{"$gt":6549.4}}]}]} 
  -> [none] FETCH: plan_stability_subjoin_cardinality_md.lineitem {"l_receiptdate":{"$not":{"$gte":"1996-12-14T00:00:00.000Z"}}} 
      -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.lineitem l_orderkey_1
```
Estimated cardinality: 44  
Actual cardinality: 24  
Orders of magnitude: 0

---
## >>> Command idx 16
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$and":[{"ps_supplycost":{"$gt":299.69}}]}},
{"$match":{"$nor":[{"ps_comment":{"$regex":{"$regex":"^ons.","$options":""}}}]}},
{"$match":{"$and":[{"ps_supplycost":{"$lt":397.14}},{"ps_availqty":{"$gt":8620}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"supplier","localField":"partsupp.ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$or":[{"s_nationkey":5},{"s_name":"Supplier#000000577"},{"s_acctbal":{"$lte":7448.46}},{"s_acctbal":{"$lt":958.07}}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$or":[{"n_name":{"$in":["MOZAMBIQUE","IRAQ","MOZAMBIQUE"]}},{"n_name":{"$nin":["JAPAN","MOROCCO"]}},{"n_name":"UNITED STATES"}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$lookup":{"from":"region","localField":"nation_s.n_regionkey","foreignField":"r_regionkey","pipeline":[
{"$match":{"$nor":[{"r_regionkey":0},{"r_regionkey":4},{"r_regionkey":4}]}}],"as":"region_s"}},
{"$unwind":"$region_s"},
{"$match":{"$or":[{"region_s.r_regionkey":1},{"region_s.r_regionkey":0},{"supplier.o_totalprice":{"$eq":162171.78}}]}}],"cursor":{},"idx":16}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 17
```
{"aggregate":"supplier","pipeline":[
{"$lookup":{"from":"nation","localField":"s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$or":[{"n_name":"GERMANY"},{"n_name":"IRAN"}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$lookup":{"from":"region","localField":"nation_s.n_regionkey","foreignField":"r_regionkey","pipeline":[
{"$match":{"$nor":[{"r_name":"AMERICA"}]}}],"as":"region_s"}},
{"$unwind":"$region_s"},
{"$match":{"$nor":[{"s_nationkey":9}]}}],"cursor":{},"idx":17}
```
### >>> Subjoin 17-0
```
db.nation.aggregate(EJSON.deserialize(
[
{"$match":{"n_name":{"$in":["GERMANY","IRAN"]}}}]
));
```
Subjoin plan:
```
COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"n_name":{"$in":["GERMANY","IRAN"]}}
```
Estimated cardinality: 2  
Actual cardinality: 2  
Orders of magnitude: 0

---
### >>> Subjoin 17-1
```
db.nation.aggregate(EJSON.deserialize(
[
{"$match":{"n_name":{"$in":["GERMANY","IRAN"]}}},
{"$lookup":{"from":"region","localField":"n_regionkey","foreignField":"r_regionkey","as":"region","pipeline":[
{"$match":{"r_name":{"$not":{"$eq":"AMERICA"}}}}]}},
{"$unwind":"$region"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$region"]}}}]
));
```
Subjoin plan:
```
HJ n_regionkey = r_regionkey
  -> [nation_s] COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"n_name":{"$in":["GERMANY","IRAN"]}} 
  -> [region_s] COLLSCAN: plan_stability_subjoin_cardinality_md.region {"r_name":{"$not":{"$eq":"AMERICA"}}}
```
Estimated cardinality: 2  
Actual cardinality: 2  
Orders of magnitude: 0

---
### >>> Subjoin 17-2
```
db.nation.aggregate(EJSON.deserialize(
[
{"$match":{"n_name":{"$in":["GERMANY","IRAN"]}}},
{"$lookup":{"from":"region","localField":"n_regionkey","foreignField":"r_regionkey","as":"region","pipeline":[
{"$match":{"r_name":{"$not":{"$eq":"AMERICA"}}}}]}},
{"$unwind":"$region"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$region"]}}},
{"$lookup":{"from":"supplier","localField":"n_nationkey","foreignField":"s_nationkey","as":"supplier","pipeline":[
{"$match":{"$and":[{"s_nationkey":{"$not":{"$eq":9}}},{}]}}]}},
{"$unwind":"$supplier"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$supplier"]}}}]
));
```
Subjoin plan:
```
INLJ nation_s.n_nationkey = s_nationkey
  -> [none] HJ n_regionkey = r_regionkey
      -> [nation_s] COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"n_name":{"$in":["GERMANY","IRAN"]}} 
      -> [region_s] COLLSCAN: plan_stability_subjoin_cardinality_md.region {"r_name":{"$not":{"$eq":"AMERICA"}}} 
  -> [none] FETCH: plan_stability_subjoin_cardinality_md.supplier {"s_nationkey":{"$not":{"$eq":9}}} 
      -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.supplier s_nationkey_1
```
Estimated cardinality: 61  
Actual cardinality: 90  
Orders of magnitude: 0

---
## >>> Command idx 18
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$and":[{"ps_availqty":{"$gte":756}},{"ps_supplycost":{"$lt":739.85}},{"ps_comment":{"$regex":{"$regex":"^a","$options":""}}},{"ps_availqty":{"$gt":724}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"supplier","localField":"partsupp.ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$nor":[{"s_name":"Supplier#000000928"},{"s_nationkey":17},{"s_name":"Supplier#000000335"},{"s_nationkey":6}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$or":[{"n_regionkey":4},{"n_regionkey":2},{"n_regionkey":0}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$lookup":{"from":"region","localField":"nation_s.n_regionkey","foreignField":"r_regionkey","pipeline":[
{"$match":{"$nor":[{"r_regionkey":3}]}}],"as":"region_s"}},
{"$unwind":"$region_s"},
{"$match":{"$or":[{"p_name":"dark goldenrod pink chiffon forest"},{"region_s.r_name":"ASIA"}]}}],"cursor":{},"idx":18}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 19
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$or":[{"ps_comment":{"$regex":{"$regex":"^s","$options":""}}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"supplier","localField":"partsupp.ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$nor":[{"s_nationkey":6}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$nor":[{"n_regionkey":4}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$lookup":{"from":"region","localField":"nation_s.n_regionkey","foreignField":"r_regionkey","pipeline":[
{"$match":{"$and":[{"r_regionkey":1}]}}],"as":"region_s"}},
{"$unwind":"$region_s"},
{"$match":{"$or":[{"region_s.r_regionkey":3},{"p_size":{"$eq":34}}]}}],"cursor":{},"idx":19}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 20
```
{"aggregate":"supplier","pipeline":[
{"$lookup":{"from":"nation","localField":"s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$or":[{"n_regionkey":4},{"n_name":{"$nin":["IRAQ","JORDAN"]}}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$lookup":{"from":"region","localField":"nation_s.n_regionkey","foreignField":"r_regionkey","pipeline":[
{"$match":{"$and":[{"r_name":"MIDDLE EAST"}]}}],"as":"region_s"}},
{"$unwind":"$region_s"},
{"$match":{"$nor":[{"region_s.r_name":"AMERICA"},{"s_acctbal":{"$lt":1922.82}}]}}],"cursor":{},"idx":20}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 21
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$or":[{"ps_supplycost":{"$gt":53.25}}]}},
{"$match":{"$and":[{"ps_availqty":{"$gt":9806}},{"ps_supplycost":{"$lte":989.18}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"supplier","localField":"partsupp.ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$nor":[{"s_name":"Supplier#000000843"},{"s_name":"Supplier#000000046"},{"s_name":"Supplier#000000563"},{"s_acctbal":{"$lt":8724.42}}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$and":[{"n_regionkey":1}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$lookup":{"from":"region","localField":"nation_s.n_regionkey","foreignField":"r_regionkey","pipeline":[
{"$match":{"$and":[{"r_name":"AMERICA"},{"r_regionkey":1}]}},
{"$match":{"$nor":[{"r_name":"AFRICA"}]}}],"as":"region_s"}},
{"$unwind":"$region_s"},
{"$match":{"$or":[{"nation_s.n_regionkey":1},{"region_s.r_name":"AMERICA"}]}}],"cursor":{},"idx":21}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 22
```
{"aggregate":"supplier","pipeline":[
{"$lookup":{"from":"nation","localField":"s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$or":[{"n_regionkey":2},{"n_regionkey":1}]}},
{"$match":{"$nor":[{"n_regionkey":1},{"n_name":{"$nin":["INDONESIA","VIETNAM","MOROCCO"]}}]}},
{"$match":{"$or":[{"n_name":{"$nin":["EGYPT","ARGENTINA","ETHIOPIA"]}},{"n_name":{"$nin":["VIETNAM","IRAQ","VIETNAM"]}}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$lookup":{"from":"region","localField":"nation_s.n_regionkey","foreignField":"r_regionkey","pipeline":[
{"$match":{"$and":[{"r_name":"ASIA"}]}}],"as":"region_s"}},
{"$unwind":"$region_s"},
{"$match":{"$nor":[{"region_s.r_regionkey":1}]}}],"cursor":{},"idx":22}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 23
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$or":[{"ps_availqty":{"$gte":2826}},{"ps_availqty":{"$lte":1414}}]}},
{"$match":{"$or":[{"ps_supplycost":{"$lte":161.52}},{"ps_availqty":{"$eq":9578}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"supplier","localField":"partsupp.ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$nor":[{"s_nationkey":22}]}},
{"$match":{"$or":[{"s_name":"Supplier#000000657"},{"s_acctbal":{"$lte":-170.22}}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$match":{"$and":[{"p_container":{"$in":["MED PACK","SM CAN","JUMBO BAG","LG CASE"]}}]}}],"cursor":{},"idx":23}
```
### >>> Subjoin 23-0
```
db.supplier.aggregate(EJSON.deserialize(
[
{"$match":{"$and":[{"$or":[{"s_name":{"$eq":"Supplier#000000657"}},{"s_acctbal":{"$lte":-170.22}}]},{"s_nationkey":{"$not":{"$eq":22}}}]}}]
));
```
Subjoin plan:
```
COLLSCAN: plan_stability_subjoin_cardinality_md.supplier {"$and":[{"$or":[{"s_name":{"$eq":"Supplier#000000657"}},{"s_acctbal":{"$lte":-170.22}}]},{"s_nationkey":{"$not":{"$eq":22}}}]}
```
Estimated cardinality: 71  
Actual cardinality: 71  
Orders of magnitude: 0

---
### >>> Subjoin 23-1
```
db.supplier.aggregate(EJSON.deserialize(
[
{"$match":{"$and":[{"$or":[{"s_name":{"$eq":"Supplier#000000657"}},{"s_acctbal":{"$lte":-170.22}}]},{"s_nationkey":{"$not":{"$eq":22}}}]}},
{"$lookup":{"from":"partsupp","localField":"s_suppkey","foreignField":"ps_suppkey","as":"partsupp","pipeline":[
{"$match":{"$and":[{"$and":[{"$or":[{"ps_availqty":{"$eq":9578}},{"ps_supplycost":{"$lte":161.52}}]},{"$or":[{"ps_availqty":{"$lte":1414}},{"ps_availqty":{"$gte":2826}}]}]},{}]}}]}},
{"$unwind":"$partsupp"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$partsupp"]}}}]
));
```
Subjoin plan:
```
INLJ s_suppkey = ps_suppkey
  -> [supplier] COLLSCAN: plan_stability_subjoin_cardinality_md.supplier {"$and":[{"$or":[{"s_name":{"$eq":"Supplier#000000657"}},{"s_acctbal":{"$lte":-170.22}}]},{"s_nationkey":{"$not":{"$eq":22}}}]} 
  -> [partsupp] FETCH: plan_stability_subjoin_cardinality_md.partsupp {"$and":[{"$or":[{"ps_availqty":{"$eq":9578}},{"ps_supplycost":{"$lte":161.52}}]},{"$or":[{"ps_availqty":{"$lte":1414}},{"ps_availqty":{"$gte":2826}}]}]} 
      -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.partsupp ps_suppkey_1
```
Estimated cardinality: 716  
Actual cardinality: 758  
Orders of magnitude: 0

---
### >>> Subjoin 23-2
```
db.supplier.aggregate(EJSON.deserialize(
[
{"$match":{"$and":[{"$or":[{"s_name":{"$eq":"Supplier#000000657"}},{"s_acctbal":{"$lte":-170.22}}]},{"s_nationkey":{"$not":{"$eq":22}}}]}},
{"$lookup":{"from":"partsupp","localField":"s_suppkey","foreignField":"ps_suppkey","as":"partsupp","pipeline":[
{"$match":{"$and":[{"$and":[{"$or":[{"ps_availqty":{"$eq":9578}},{"ps_supplycost":{"$lte":161.52}}]},{"$or":[{"ps_availqty":{"$lte":1414}},{"ps_availqty":{"$gte":2826}}]}]},{}]}}]}},
{"$unwind":"$partsupp"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$partsupp"]}}},
{"$lookup":{"from":"part","localField":"ps_partkey","foreignField":"p_partkey","as":"part","pipeline":[
{"$match":{"p_container":{"$in":["JUMBO BAG","LG CASE","MED PACK","SM CAN"]}}}]}},
{"$unwind":"$part"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$part"]}}}]
));
```
Subjoin plan:
```
HJ partsupp.ps_partkey = p_partkey
  -> [none] INLJ s_suppkey = ps_suppkey
      -> [supplier] COLLSCAN: plan_stability_subjoin_cardinality_md.supplier {"$and":[{"$or":[{"s_name":{"$eq":"Supplier#000000657"}},{"s_acctbal":{"$lte":-170.22}}]},{"s_nationkey":{"$not":{"$eq":22}}}]} 
      -> [partsupp] FETCH: plan_stability_subjoin_cardinality_md.partsupp {"$and":[{"$or":[{"ps_availqty":{"$eq":9578}},{"ps_supplycost":{"$lte":161.52}}]},{"$or":[{"ps_availqty":{"$lte":1414}},{"ps_availqty":{"$gte":2826}}]}]} 
          -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.partsupp ps_suppkey_1
  -> [none] COLLSCAN: plan_stability_subjoin_cardinality_md.part {"p_container":{"$in":["JUMBO BAG","LG CASE","MED PACK","SM CAN"]}}
```
Estimated cardinality: 72  
Actual cardinality: 69  
Orders of magnitude: 0

---
## >>> Command idx 24
```
{"aggregate":"lineitem","pipeline":[
{"$lookup":{"from":"orders","localField":"l_orderkey","foreignField":"o_orderkey","pipeline":[
{"$match":{"$and":[{"o_orderdate":{"$gt":"1998-07-29T00:00:00.000Z"}},{"o_orderstatus":"O"},{"o_orderdate":{"$gte":"1994-09-13T00:00:00.000Z"}}]}}],"as":"orders"}},
{"$unwind":"$orders"},
{"$lookup":{"from":"customer","localField":"orders.o_custkey","foreignField":"c_custkey","pipeline":[
{"$match":{"$nor":[{"c_name":"Customer#000010639"}]}},
{"$match":{"$and":[{"c_mktsegment":"HOUSEHOLD"}]}}],"as":"customer"}},
{"$unwind":"$customer"},
{"$lookup":{"from":"supplier","localField":"customer.c_nationkey","foreignField":"s_nationkey","pipeline":[
{"$match":{"$nor":[{"s_acctbal":{"$lte":6399.78}},{"s_acctbal":{"$gte":7619.85}}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$match":{"$nor":[{"supplier.o_orderstatus":"F"}]}}],"cursor":{},"idx":24}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 25
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$and":[{"ps_comment":{"$regex":{"$regex":"^. de","$options":""}}}]}},
{"$match":{"$nor":[{"ps_availqty":{"$gte":8163}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"supplier","localField":"partsupp.ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$or":[{"s_acctbal":{"$lte":7082.37}}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$and":[{"n_regionkey":3}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$lookup":{"from":"region","localField":"nation_s.n_regionkey","foreignField":"r_regionkey","pipeline":[
{"$match":{"$nor":[{"r_regionkey":4}]}}],"as":"region_s"}},
{"$unwind":"$region_s"},
{"$match":{"$nor":[{"p_partkey":5801}]}}],"cursor":{},"idx":25}
```
### >>> Subjoin 25-0
```
db.region.aggregate(EJSON.deserialize(
[
{"$match":{"r_regionkey":{"$not":{"$eq":4}}}}]
));
```
Subjoin plan:
```
COLLSCAN: plan_stability_subjoin_cardinality_md.region {"r_regionkey":{"$not":{"$eq":4}}}
```
Estimated cardinality: 4  
Actual cardinality: 4  
Orders of magnitude: 0

---
### >>> Subjoin 25-1
```
db.region.aggregate(EJSON.deserialize(
[
{"$match":{"r_regionkey":{"$not":{"$eq":4}}}},
{"$lookup":{"from":"nation","localField":"r_regionkey","foreignField":"n_regionkey","as":"nation","pipeline":[
{"$match":{"$and":[{"n_regionkey":{"$eq":3}},{}]}}]}},
{"$unwind":"$nation"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$nation"]}}}]
));
```
Subjoin plan:
```
INLJ r_regionkey = n_regionkey
  -> [region_s] COLLSCAN: plan_stability_subjoin_cardinality_md.region {"r_regionkey":{"$not":{"$eq":4}}} 
  -> [nation_s] FETCH: plan_stability_subjoin_cardinality_md.nation {"n_regionkey":{"$eq":3}} 
      -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.nation n_regionkey_1
```
Estimated cardinality: 4  
Actual cardinality: 5  
Orders of magnitude: 0

---
### >>> Subjoin 25-2
```
db.region.aggregate(EJSON.deserialize(
[
{"$match":{"r_regionkey":{"$not":{"$eq":4}}}},
{"$lookup":{"from":"nation","localField":"r_regionkey","foreignField":"n_regionkey","as":"nation","pipeline":[
{"$match":{"$and":[{"n_regionkey":{"$eq":3}},{}]}}]}},
{"$unwind":"$nation"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$nation"]}}},
{"$lookup":{"from":"supplier","localField":"n_nationkey","foreignField":"s_nationkey","as":"supplier","pipeline":[
{"$match":{"s_acctbal":{"$lte":7082.37}}}]}},
{"$unwind":"$supplier"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$supplier"]}}}]
));
```
Subjoin plan:
```
HJ nation_s.n_nationkey = s_nationkey
  -> [none] INLJ r_regionkey = n_regionkey
      -> [region_s] COLLSCAN: plan_stability_subjoin_cardinality_md.region {"r_regionkey":{"$not":{"$eq":4}}} 
      -> [nation_s] FETCH: plan_stability_subjoin_cardinality_md.nation {"n_regionkey":{"$eq":3}} 
          -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.nation n_regionkey_1
  -> [supplier] COLLSCAN: plan_stability_subjoin_cardinality_md.supplier {"s_acctbal":{"$lte":7082.37}}
```
Estimated cardinality: 118  
Actual cardinality: 153  
Orders of magnitude: 0

---
### >>> Subjoin 25-3
```
db.region.aggregate(EJSON.deserialize(
[
{"$match":{"r_regionkey":{"$not":{"$eq":4}}}},
{"$lookup":{"from":"nation","localField":"r_regionkey","foreignField":"n_regionkey","as":"nation","pipeline":[
{"$match":{"$and":[{"n_regionkey":{"$eq":3}},{}]}}]}},
{"$unwind":"$nation"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$nation"]}}},
{"$lookup":{"from":"supplier","localField":"n_nationkey","foreignField":"s_nationkey","as":"supplier","pipeline":[
{"$match":{"s_acctbal":{"$lte":7082.37}}}]}},
{"$unwind":"$supplier"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$supplier"]}}},
{"$lookup":{"from":"partsupp","localField":"s_suppkey","foreignField":"ps_suppkey","as":"partsupp","pipeline":[
{"$match":{"$and":[{"$and":[{"ps_comment":{"$regex":"^. de"}},{"ps_availqty":{"$not":{"$gte":8163}}}]},{}]}}]}},
{"$unwind":"$partsupp"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$partsupp"]}}}]
));
```
Subjoin plan:
```
INLJ supplier.s_suppkey = ps_suppkey
  -> [none] HJ nation_s.n_nationkey = s_nationkey
      -> [none] INLJ r_regionkey = n_regionkey
          -> [region_s] COLLSCAN: plan_stability_subjoin_cardinality_md.region {"r_regionkey":{"$not":{"$eq":4}}} 
          -> [nation_s] FETCH: plan_stability_subjoin_cardinality_md.nation {"n_regionkey":{"$eq":3}} 
              -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.nation n_regionkey_1
      -> [supplier] COLLSCAN: plan_stability_subjoin_cardinality_md.supplier {"s_acctbal":{"$lte":7082.37}} 
  -> [partsupp] FETCH: plan_stability_subjoin_cardinality_md.partsupp {"$and":[{"ps_comment":{"$regex":"^. de"}},{"ps_availqty":{"$not":{"$gte":8163}}}]} 
      -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.partsupp ps_suppkey_1
```
Estimated cardinality: 28  
Actual cardinality: 50  
Orders of magnitude: 0

---
### >>> Subjoin 25-4
```
db.region.aggregate(EJSON.deserialize(
[
{"$match":{"r_regionkey":{"$not":{"$eq":4}}}},
{"$lookup":{"from":"nation","localField":"r_regionkey","foreignField":"n_regionkey","as":"nation","pipeline":[
{"$match":{"$and":[{"n_regionkey":{"$eq":3}},{}]}}]}},
{"$unwind":"$nation"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$nation"]}}},
{"$lookup":{"from":"supplier","localField":"n_nationkey","foreignField":"s_nationkey","as":"supplier","pipeline":[
{"$match":{"s_acctbal":{"$lte":7082.37}}}]}},
{"$unwind":"$supplier"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$supplier"]}}},
{"$lookup":{"from":"partsupp","localField":"s_suppkey","foreignField":"ps_suppkey","as":"partsupp","pipeline":[
{"$match":{"$and":[{"$and":[{"ps_comment":{"$regex":"^. de"}},{"ps_availqty":{"$not":{"$gte":8163}}}]},{}]}}]}},
{"$unwind":"$partsupp"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$partsupp"]}}},
{"$lookup":{"from":"part","localField":"ps_partkey","foreignField":"p_partkey","as":"part","pipeline":[
{"$match":{"$and":[{"p_partkey":{"$not":{"$eq":5801}}},{}]}}]}},
{"$unwind":"$part"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$part"]}}}]
));
```
Subjoin plan:
```
INLJ partsupp.ps_partkey = p_partkey
  -> [none] INLJ supplier.s_suppkey = ps_suppkey
      -> [none] HJ nation_s.n_nationkey = s_nationkey
          -> [none] INLJ r_regionkey = n_regionkey
              -> [region_s] COLLSCAN: plan_stability_subjoin_cardinality_md.region {"r_regionkey":{"$not":{"$eq":4}}} 
              -> [nation_s] FETCH: plan_stability_subjoin_cardinality_md.nation {"n_regionkey":{"$eq":3}} 
                  -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.nation n_regionkey_1
          -> [supplier] COLLSCAN: plan_stability_subjoin_cardinality_md.supplier {"s_acctbal":{"$lte":7082.37}} 
      -> [partsupp] FETCH: plan_stability_subjoin_cardinality_md.partsupp {"$and":[{"ps_comment":{"$regex":"^. de"}},{"ps_availqty":{"$not":{"$gte":8163}}}]} 
          -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.partsupp ps_suppkey_1
  -> [none] FETCH: plan_stability_subjoin_cardinality_md.part {"p_partkey":{"$not":{"$eq":5801}}} 
      -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.part p_partkey_1
```
Estimated cardinality: 28  
Actual cardinality: 50  
Orders of magnitude: 0

---
## >>> Command idx 26
```
{"aggregate":"supplier","pipeline":[
{"$lookup":{"from":"nation","localField":"s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$nor":[{"n_regionkey":2},{"n_name":"PERU"},{"n_regionkey":1}]}},
{"$match":{"$or":[{"n_name":{"$nin":["MOZAMBIQUE","INDIA"]}},{"n_regionkey":3}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$lookup":{"from":"region","localField":"nation_s.n_regionkey","foreignField":"r_regionkey","pipeline":[
{"$match":{"$nor":[{"r_regionkey":0},{"r_name":"EUROPE"},{"r_name":"AFRICA"}]}}],"as":"region_s"}},
{"$unwind":"$region_s"},
{"$match":{"$or":[{"s_nationkey":10},{"nation_s.n_regionkey":2}]}}],"cursor":{},"idx":26}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 27
```
{"aggregate":"partsupp","pipeline":[
{"$lookup":{"from":"supplier","localField":"ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$or":[{"s_name":"Supplier#000000103"},{"s_nationkey":23},{"s_name":"Supplier#000000785"},{"s_name":"Supplier#000000478"}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$nor":[{"n_name":"KENYA"},{"n_regionkey":2}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$lookup":{"from":"region","localField":"nation_s.n_regionkey","foreignField":"r_regionkey","pipeline":[
{"$match":{"$or":[{"r_name":"AMERICA"}]}}],"as":"region_s"}},
{"$unwind":"$region_s"},
{"$match":{"$nor":[{"region_s.r_name":"EUROPE"}]}}],"cursor":{},"idx":27}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 28
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$nor":[{"ps_supplycost":{"$gte":495.17}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"lineitem","localField":"partsupp.ps_partkey","foreignField":"l_partkey","pipeline":[
{"$match":{"$and":[{"l_linenumber":{"$gt":4}},{"l_shipmode":{"$in":["REG AIR","REG AIR"]}}]}},
{"$match":{"$nor":[{"l_partkey":6234}]}}],"as":"lineitem"}},
{"$unwind":"$lineitem"},
{"$match":{"$or":[{"p_comment":{"$regex":{"$regex":"^ r","$options":""}}}]}}],"cursor":{},"idx":28}
```
### >>> Subjoin 28-0
```
db.part.aggregate(EJSON.deserialize(
[
{"$match":{"p_comment":{"$regex":"^ r"}}}]
));
```
Subjoin plan:
```
COLLSCAN: plan_stability_subjoin_cardinality_md.part {"p_comment":{"$regex":"^ r"}}
```
Estimated cardinality: 180  
Actual cardinality: 201  
Orders of magnitude: 0

---
### >>> Subjoin 28-1
```
db.part.aggregate(EJSON.deserialize(
[
{"$match":{"p_comment":{"$regex":"^ r"}}},
{"$lookup":{"from":"lineitem","localField":"p_partkey","foreignField":"l_partkey","as":"lineitem","pipeline":[
{"$match":{"$and":[{"$and":[{"l_shipmode":{"$eq":"REG AIR"}},{"l_linenumber":{"$gt":4}},{"l_partkey":{"$not":{"$eq":6234}}}]},{}]}}]}},
{"$unwind":"$lineitem"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$lineitem"]}}}]
));
```
Subjoin plan:
```
INLJ p_partkey = l_partkey
  -> [none] COLLSCAN: plan_stability_subjoin_cardinality_md.part {"p_comment":{"$regex":"^ r"}} 
  -> [lineitem] FETCH: plan_stability_subjoin_cardinality_md.lineitem {"$and":[{"l_shipmode":{"$eq":"REG AIR"}},{"l_linenumber":{"$gt":4}},{"l_partkey":{"$not":{"$eq":6234}}}]} 
      -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.lineitem l_partkey_1
```
Estimated cardinality: 168  
Actual cardinality: 169  
Orders of magnitude: 0

---
### >>> Subjoin 28-2
```
db.part.aggregate(EJSON.deserialize(
[
{"$match":{"p_comment":{"$regex":"^ r"}}},
{"$lookup":{"from":"lineitem","localField":"p_partkey","foreignField":"l_partkey","as":"lineitem","pipeline":[
{"$match":{"$and":[{"$and":[{"l_shipmode":{"$eq":"REG AIR"}},{"l_linenumber":{"$gt":4}},{"l_partkey":{"$not":{"$eq":6234}}}]},{}]}}]}},
{"$unwind":"$lineitem"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$lineitem"]}}},
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","as":"partsupp","pipeline":[
{"$match":{"$and":[{"ps_supplycost":{"$not":{"$gte":495.17}}},{}]}}]}},
{"$unwind":"$partsupp"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$partsupp"]}}}]
));
```
Subjoin plan:
```
INLJ p_partkey = ps_partkey, lineitem.l_partkey = ps_partkey
  -> [none] INLJ p_partkey = l_partkey
      -> [none] COLLSCAN: plan_stability_subjoin_cardinality_md.part {"p_comment":{"$regex":"^ r"}} 
      -> [lineitem] FETCH: plan_stability_subjoin_cardinality_md.lineitem {"$and":[{"l_shipmode":{"$eq":"REG AIR"}},{"l_linenumber":{"$gt":4}},{"l_partkey":{"$not":{"$eq":6234}}}]} 
          -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.lineitem l_partkey_1
  -> [partsupp] FETCH: plan_stability_subjoin_cardinality_md.partsupp {"ps_supplycost":{"$not":{"$gte":495.17}}} 
      -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.partsupp ps_partkey_1
```
Estimated cardinality: 26225  
Actual cardinality: 358  
Orders of magnitude: 2
> [!WARNING]
> Estimate discrepancy is more than 2 orders of magnitude.

---
## >>> Command idx 29
```
{"aggregate":"supplier","pipeline":[
{"$lookup":{"from":"nation","localField":"s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$or":[{"n_name":{"$nin":["MOROCCO","SAUDI ARABIA"]}}]}},
{"$match":{"$nor":[{"n_regionkey":0}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$lookup":{"from":"region","localField":"nation_s.n_regionkey","foreignField":"r_regionkey","pipeline":[
{"$match":{"$nor":[{"r_regionkey":1}]}}],"as":"region_s"}},
{"$unwind":"$region_s"},
{"$match":{"$and":[{"s_acctbal":{"$gt":8724.42}}]}}],"cursor":{},"idx":29}
```
### >>> Subjoin 29-0
```
db.region.aggregate(EJSON.deserialize(
[
{"$match":{"r_regionkey":{"$not":{"$eq":1}}}}]
));
```
Subjoin plan:
```
COLLSCAN: plan_stability_subjoin_cardinality_md.region {"r_regionkey":{"$not":{"$eq":1}}}
```
Estimated cardinality: 4  
Actual cardinality: 4  
Orders of magnitude: 0

---
### >>> Subjoin 29-1
```
db.region.aggregate(EJSON.deserialize(
[
{"$match":{"r_regionkey":{"$not":{"$eq":1}}}},
{"$lookup":{"from":"nation","localField":"r_regionkey","foreignField":"n_regionkey","as":"nation","pipeline":[
{"$match":{"$and":[{"n_regionkey":{"$not":{"$eq":0}}},{"n_name":{"$not":{"$in":["MOROCCO","SAUDI ARABIA"]}}}]}}]}},
{"$unwind":"$nation"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$nation"]}}}]
));
```
Subjoin plan:
```
HJ r_regionkey = n_regionkey
  -> [region_s] COLLSCAN: plan_stability_subjoin_cardinality_md.region {"r_regionkey":{"$not":{"$eq":1}}} 
  -> [nation_s] COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"$and":[{"n_regionkey":{"$not":{"$eq":0}}},{"n_name":{"$not":{"$in":["MOROCCO","SAUDI ARABIA"]}}}]}
```
Estimated cardinality: 15  
Actual cardinality: 14  
Orders of magnitude: 0

---
### >>> Subjoin 29-2
```
db.region.aggregate(EJSON.deserialize(
[
{"$match":{"r_regionkey":{"$not":{"$eq":1}}}},
{"$lookup":{"from":"nation","localField":"r_regionkey","foreignField":"n_regionkey","as":"nation","pipeline":[
{"$match":{"$and":[{"n_regionkey":{"$not":{"$eq":0}}},{"n_name":{"$not":{"$in":["MOROCCO","SAUDI ARABIA"]}}}]}}]}},
{"$unwind":"$nation"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$nation"]}}},
{"$lookup":{"from":"supplier","localField":"n_nationkey","foreignField":"s_nationkey","as":"supplier","pipeline":[
{"$match":{"s_acctbal":{"$gt":8724.42}}}]}},
{"$unwind":"$supplier"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$supplier"]}}}]
));
```
Subjoin plan:
```
HJ nation_s.n_nationkey = s_nationkey
  -> [none] HJ r_regionkey = n_regionkey
      -> [region_s] COLLSCAN: plan_stability_subjoin_cardinality_md.region {"r_regionkey":{"$not":{"$eq":1}}} 
      -> [nation_s] COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"$and":[{"n_regionkey":{"$not":{"$eq":0}}},{"n_name":{"$not":{"$in":["MOROCCO","SAUDI ARABIA"]}}}]} 
  -> [none] COLLSCAN: plan_stability_subjoin_cardinality_md.supplier {"s_acctbal":{"$gt":8724.42}}
```
Estimated cardinality: 73  
Actual cardinality: 62  
Orders of magnitude: 0

---
## >>> Command idx 30
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$or":[{"ps_comment":{"$regex":{"$regex":"^ s","$options":""}}},{"ps_supplycost":{"$gte":597.44}}]}},
{"$match":{"$and":[{"ps_comment":{"$regex":{"$regex":"^. ","$options":""}}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"lineitem","localField":"partsupp.ps_partkey","foreignField":"l_partkey","pipeline":[
{"$match":{"$nor":[{"l_extendedprice":{"$gt":46445.4}},{"l_returnflag":"R"}]}},
{"$match":{"$or":[{"l_discount":{"$lt":0.07}},{"l_quantity":{"$lte":46}}]}}],"as":"lineitem"}},
{"$unwind":"$lineitem"},
{"$match":{"$nor":[{"lineitem.l_suppkey":169},{"p_mfgr":{"$in":["Manufacturer#2","Manufacturer#5","Manufacturer#3"]}},{"lineitem.l_linenumber":{"$lte":6}}]}}],"cursor":{},"idx":30}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 31
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$or":[{"ps_supplycost":{"$lte":285.82}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"supplier","localField":"partsupp.ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$and":[{"s_acctbal":{"$lte":-686.97}}]}},
{"$match":{"$or":[{"s_acctbal":{"$lte":5704.81}},{"s_name":"Supplier#000000338"}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$nor":[{"n_name":"UNITED STATES"},{"n_name":"IRAQ"}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$lookup":{"from":"region","localField":"nation_s.n_regionkey","foreignField":"r_regionkey","pipeline":[
{"$match":{"$nor":[{"r_name":"AMERICA"},{"r_name":"ASIA"}]}},
{"$match":{"$nor":[{"r_regionkey":4},{"r_name":"AFRICA"}]}},
{"$match":{"$nor":[{"r_name":"MIDDLE EAST"},{"r_regionkey":1}]}}],"as":"region_s"}},
{"$unwind":"$region_s"},
{"$match":{"$and":[{"p_container":{"$in":["MED BOX","MED PACK"]}}]}}],"cursor":{},"idx":31}
```
### >>> Subjoin 31-0
```
db.region.aggregate(EJSON.deserialize(
[
{"$match":{"$and":[{"$nor":[{"r_name":{"$eq":"AMERICA"}},{"r_name":{"$eq":"ASIA"}}]},{"$nor":[{"r_name":{"$eq":"AFRICA"}},{"r_regionkey":{"$eq":4}}]},{"$nor":[{"r_name":{"$eq":"MIDDLE EAST"}},{"r_regionkey":{"$eq":1}}]}]}}]
));
```
Subjoin plan:
```
COLLSCAN: plan_stability_subjoin_cardinality_md.region {"$and":[{"$nor":[{"r_name":{"$eq":"AMERICA"}},{"r_name":{"$eq":"ASIA"}}]},{"$nor":[{"r_name":{"$eq":"AFRICA"}},{"r_regionkey":{"$eq":4}}]},{"$nor":[{"r_name":{"$eq":"MIDDLE EAST"}},{"r_regionkey":{"$eq":1}}]}]}
```
Estimated cardinality: 1  
Actual cardinality: 1  
Orders of magnitude: 0

---
### >>> Subjoin 31-1
```
db.region.aggregate(EJSON.deserialize(
[
{"$match":{"$and":[{"$nor":[{"r_name":{"$eq":"AMERICA"}},{"r_name":{"$eq":"ASIA"}}]},{"$nor":[{"r_name":{"$eq":"AFRICA"}},{"r_regionkey":{"$eq":4}}]},{"$nor":[{"r_name":{"$eq":"MIDDLE EAST"}},{"r_regionkey":{"$eq":1}}]}]}},
{"$lookup":{"from":"nation","localField":"r_regionkey","foreignField":"n_regionkey","as":"nation","pipeline":[
{"$match":{"$nor":[{"n_name":{"$eq":"UNITED STATES"}},{"n_name":{"$eq":"IRAQ"}}]}}]}},
{"$unwind":"$nation"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$nation"]}}}]
));
```
Subjoin plan:
```
NLJ r_regionkey = n_regionkey
  -> [region_s] COLLSCAN: plan_stability_subjoin_cardinality_md.region {"$and":[{"$nor":[{"r_name":{"$eq":"AMERICA"}},{"r_name":{"$eq":"ASIA"}}]},{"$nor":[{"r_name":{"$eq":"AFRICA"}},{"r_regionkey":{"$eq":4}}]},{"$nor":[{"r_name":{"$eq":"MIDDLE EAST"}},{"r_regionkey":{"$eq":1}}]}]} 
  -> [nation_s] COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"$nor":[{"n_name":{"$eq":"UNITED STATES"}},{"n_name":{"$eq":"IRAQ"}}]}
```
Estimated cardinality: 5  
Actual cardinality: 5  
Orders of magnitude: 0

---
### >>> Subjoin 31-2
```
db.region.aggregate(EJSON.deserialize(
[
{"$match":{"$and":[{"$nor":[{"r_name":{"$eq":"AMERICA"}},{"r_name":{"$eq":"ASIA"}}]},{"$nor":[{"r_name":{"$eq":"AFRICA"}},{"r_regionkey":{"$eq":4}}]},{"$nor":[{"r_name":{"$eq":"MIDDLE EAST"}},{"r_regionkey":{"$eq":1}}]}]}},
{"$lookup":{"from":"nation","localField":"r_regionkey","foreignField":"n_regionkey","as":"nation","pipeline":[
{"$match":{"$nor":[{"n_name":{"$eq":"UNITED STATES"}},{"n_name":{"$eq":"IRAQ"}}]}}]}},
{"$unwind":"$nation"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$nation"]}}},
{"$lookup":{"from":"supplier","localField":"n_nationkey","foreignField":"s_nationkey","as":"supplier","pipeline":[
{"$match":{"$and":[{"$or":[{"s_name":{"$eq":"Supplier#000000338"}},{"s_acctbal":{"$lte":5704.81}}]},{"s_acctbal":{"$lte":-686.97}}]}}]}},
{"$unwind":"$supplier"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$supplier"]}}}]
));
```
Subjoin plan:
```
HJ nation_s.n_nationkey = s_nationkey
  -> [none] NLJ r_regionkey = n_regionkey
      -> [region_s] COLLSCAN: plan_stability_subjoin_cardinality_md.region {"$and":[{"$nor":[{"r_name":{"$eq":"AMERICA"}},{"r_name":{"$eq":"ASIA"}}]},{"$nor":[{"r_name":{"$eq":"AFRICA"}},{"r_regionkey":{"$eq":4}}]},{"$nor":[{"r_name":{"$eq":"MIDDLE EAST"}},{"r_regionkey":{"$eq":1}}]}]} 
      -> [nation_s] COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"$nor":[{"n_name":{"$eq":"UNITED STATES"}},{"n_name":{"$eq":"IRAQ"}}]} 
  -> [supplier] COLLSCAN: plan_stability_subjoin_cardinality_md.supplier {"$and":[{"$or":[{"s_name":{"$eq":"Supplier#000000338"}},{"s_acctbal":{"$lte":5704.81}}]},{"s_acctbal":{"$lte":-686.97}}]}
```
Estimated cardinality: 5  
Actual cardinality: 6  
Orders of magnitude: 0

---
### >>> Subjoin 31-3
```
db.region.aggregate(EJSON.deserialize(
[
{"$match":{"$and":[{"$nor":[{"r_name":{"$eq":"AMERICA"}},{"r_name":{"$eq":"ASIA"}}]},{"$nor":[{"r_name":{"$eq":"AFRICA"}},{"r_regionkey":{"$eq":4}}]},{"$nor":[{"r_name":{"$eq":"MIDDLE EAST"}},{"r_regionkey":{"$eq":1}}]}]}},
{"$lookup":{"from":"nation","localField":"r_regionkey","foreignField":"n_regionkey","as":"nation","pipeline":[
{"$match":{"$nor":[{"n_name":{"$eq":"UNITED STATES"}},{"n_name":{"$eq":"IRAQ"}}]}}]}},
{"$unwind":"$nation"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$nation"]}}},
{"$lookup":{"from":"supplier","localField":"n_nationkey","foreignField":"s_nationkey","as":"supplier","pipeline":[
{"$match":{"$and":[{"$or":[{"s_name":{"$eq":"Supplier#000000338"}},{"s_acctbal":{"$lte":5704.81}}]},{"s_acctbal":{"$lte":-686.97}}]}}]}},
{"$unwind":"$supplier"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$supplier"]}}},
{"$lookup":{"from":"partsupp","localField":"s_suppkey","foreignField":"ps_suppkey","as":"partsupp","pipeline":[
{"$match":{"$and":[{"ps_supplycost":{"$lte":285.82}},{}]}}]}},
{"$unwind":"$partsupp"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$partsupp"]}}}]
));
```
Subjoin plan:
```
INLJ supplier.s_suppkey = ps_suppkey
  -> [none] HJ nation_s.n_nationkey = s_nationkey
      -> [none] NLJ r_regionkey = n_regionkey
          -> [region_s] COLLSCAN: plan_stability_subjoin_cardinality_md.region {"$and":[{"$nor":[{"r_name":{"$eq":"AMERICA"}},{"r_name":{"$eq":"ASIA"}}]},{"$nor":[{"r_name":{"$eq":"AFRICA"}},{"r_regionkey":{"$eq":4}}]},{"$nor":[{"r_name":{"$eq":"MIDDLE EAST"}},{"r_regionkey":{"$eq":1}}]}]} 
          -> [nation_s] COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"$nor":[{"n_name":{"$eq":"UNITED STATES"}},{"n_name":{"$eq":"IRAQ"}}]} 
      -> [supplier] COLLSCAN: plan_stability_subjoin_cardinality_md.supplier {"$and":[{"$or":[{"s_name":{"$eq":"Supplier#000000338"}},{"s_acctbal":{"$lte":5704.81}}]},{"s_acctbal":{"$lte":-686.97}}]} 
  -> [partsupp] FETCH: plan_stability_subjoin_cardinality_md.partsupp {"ps_supplycost":{"$lte":285.82}} 
      -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.partsupp ps_suppkey_1
```
Estimated cardinality: 112  
Actual cardinality: 142  
Orders of magnitude: 0

---
### >>> Subjoin 31-4
```
db.region.aggregate(EJSON.deserialize(
[
{"$match":{"$and":[{"$nor":[{"r_name":{"$eq":"AMERICA"}},{"r_name":{"$eq":"ASIA"}}]},{"$nor":[{"r_name":{"$eq":"AFRICA"}},{"r_regionkey":{"$eq":4}}]},{"$nor":[{"r_name":{"$eq":"MIDDLE EAST"}},{"r_regionkey":{"$eq":1}}]}]}},
{"$lookup":{"from":"nation","localField":"r_regionkey","foreignField":"n_regionkey","as":"nation","pipeline":[
{"$match":{"$nor":[{"n_name":{"$eq":"UNITED STATES"}},{"n_name":{"$eq":"IRAQ"}}]}}]}},
{"$unwind":"$nation"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$nation"]}}},
{"$lookup":{"from":"supplier","localField":"n_nationkey","foreignField":"s_nationkey","as":"supplier","pipeline":[
{"$match":{"$and":[{"$or":[{"s_name":{"$eq":"Supplier#000000338"}},{"s_acctbal":{"$lte":5704.81}}]},{"s_acctbal":{"$lte":-686.97}}]}}]}},
{"$unwind":"$supplier"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$supplier"]}}},
{"$lookup":{"from":"partsupp","localField":"s_suppkey","foreignField":"ps_suppkey","as":"partsupp","pipeline":[
{"$match":{"$and":[{"ps_supplycost":{"$lte":285.82}},{}]}}]}},
{"$unwind":"$partsupp"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$partsupp"]}}},
{"$lookup":{"from":"part","localField":"ps_partkey","foreignField":"p_partkey","as":"part","pipeline":[
{"$match":{"p_container":{"$in":["MED BOX","MED PACK"]}}}]}},
{"$unwind":"$part"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$part"]}}}]
));
```
Subjoin plan:
```
HJ partsupp.ps_partkey = p_partkey
  -> [none] INLJ supplier.s_suppkey = ps_suppkey
      -> [none] HJ nation_s.n_nationkey = s_nationkey
          -> [none] NLJ r_regionkey = n_regionkey
              -> [region_s] COLLSCAN: plan_stability_subjoin_cardinality_md.region {"$and":[{"$nor":[{"r_name":{"$eq":"AMERICA"}},{"r_name":{"$eq":"ASIA"}}]},{"$nor":[{"r_name":{"$eq":"AFRICA"}},{"r_regionkey":{"$eq":4}}]},{"$nor":[{"r_name":{"$eq":"MIDDLE EAST"}},{"r_regionkey":{"$eq":1}}]}]} 
              -> [nation_s] COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"$nor":[{"n_name":{"$eq":"UNITED STATES"}},{"n_name":{"$eq":"IRAQ"}}]} 
          -> [supplier] COLLSCAN: plan_stability_subjoin_cardinality_md.supplier {"$and":[{"$or":[{"s_name":{"$eq":"Supplier#000000338"}},{"s_acctbal":{"$lte":5704.81}}]},{"s_acctbal":{"$lte":-686.97}}]} 
      -> [partsupp] FETCH: plan_stability_subjoin_cardinality_md.partsupp {"ps_supplycost":{"$lte":285.82}} 
          -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.partsupp ps_suppkey_1
  -> [none] COLLSCAN: plan_stability_subjoin_cardinality_md.part {"p_container":{"$in":["MED BOX","MED PACK"]}}
```
Estimated cardinality: 5  
Actual cardinality: 10  
Orders of magnitude: 1

---
## >>> Command idx 32
```
{"aggregate":"partsupp","pipeline":[
{"$lookup":{"from":"supplier","localField":"ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$nor":[{"s_nationkey":1},{"s_nationkey":14},{"s_acctbal":{"$gte":2060.13}}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$nor":[{"n_name":{"$nin":["CHINA","SAUDI ARABIA","ROMANIA"]}},{"n_name":{"$in":["INDONESIA","EGYPT"]}},{"n_regionkey":1},{"n_name":"ALGERIA"}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$lookup":{"from":"region","localField":"nation_s.n_regionkey","foreignField":"r_regionkey","pipeline":[
{"$match":{"$and":[{"r_regionkey":4}]}}],"as":"region_s"}},
{"$unwind":"$region_s"},
{"$match":{"$or":[{"ps_comment":{"$regex":{"$regex":"^he","$options":""}}}]}}],"cursor":{},"idx":32}
```
### >>> Subjoin 32-0
```
db.nation.aggregate(EJSON.deserialize(
[
{"$match":{"$nor":[{"n_name":{"$eq":"ALGERIA"}},{"n_regionkey":{"$eq":1}},{"n_name":{"$in":["EGYPT","INDONESIA"]}},{"n_name":{"$not":{"$in":["CHINA","ROMANIA","SAUDI ARABIA"]}}}]}}]
));
```
Subjoin plan:
```
COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"$nor":[{"n_name":{"$eq":"ALGERIA"}},{"n_regionkey":{"$eq":1}},{"n_name":{"$in":["EGYPT","INDONESIA"]}},{"n_name":{"$not":{"$in":["CHINA","ROMANIA","SAUDI ARABIA"]}}}]}
```
Estimated cardinality: 3  
Actual cardinality: 3  
Orders of magnitude: 0

---
### >>> Subjoin 32-1
```
db.nation.aggregate(EJSON.deserialize(
[
{"$match":{"$nor":[{"n_name":{"$eq":"ALGERIA"}},{"n_regionkey":{"$eq":1}},{"n_name":{"$in":["EGYPT","INDONESIA"]}},{"n_name":{"$not":{"$in":["CHINA","ROMANIA","SAUDI ARABIA"]}}}]}},
{"$lookup":{"from":"region","localField":"n_regionkey","foreignField":"r_regionkey","as":"region","pipeline":[
{"$match":{"$and":[{"r_regionkey":{"$eq":4}},{}]}}]}},
{"$unwind":"$region"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$region"]}}}]
));
```
Subjoin plan:
```
INLJ n_regionkey = r_regionkey
  -> [nation_s] COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"$nor":[{"n_name":{"$eq":"ALGERIA"}},{"n_regionkey":{"$eq":1}},{"n_name":{"$in":["EGYPT","INDONESIA"]}},{"n_name":{"$not":{"$in":["CHINA","ROMANIA","SAUDI ARABIA"]}}}]} 
  -> [region_s] FETCH: plan_stability_subjoin_cardinality_md.region {"r_regionkey":{"$eq":4}} 
      -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.region r_regionkey_1
```
Estimated cardinality: 1  
Actual cardinality: 1  
Orders of magnitude: 0

---
### >>> Subjoin 32-2
```
db.nation.aggregate(EJSON.deserialize(
[
{"$match":{"$nor":[{"n_name":{"$eq":"ALGERIA"}},{"n_regionkey":{"$eq":1}},{"n_name":{"$in":["EGYPT","INDONESIA"]}},{"n_name":{"$not":{"$in":["CHINA","ROMANIA","SAUDI ARABIA"]}}}]}},
{"$lookup":{"from":"region","localField":"n_regionkey","foreignField":"r_regionkey","as":"region","pipeline":[
{"$match":{"$and":[{"r_regionkey":{"$eq":4}},{}]}}]}},
{"$unwind":"$region"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$region"]}}},
{"$lookup":{"from":"supplier","localField":"n_nationkey","foreignField":"s_nationkey","as":"supplier","pipeline":[
{"$match":{"$and":[{"$nor":[{"s_nationkey":{"$eq":1}},{"s_nationkey":{"$eq":14}},{"s_acctbal":{"$gte":2060.13}}]},{}]}}]}},
{"$unwind":"$supplier"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$supplier"]}}}]
));
```
Subjoin plan:
```
INLJ nation_s.n_nationkey = s_nationkey
  -> [none] INLJ n_regionkey = r_regionkey
      -> [nation_s] COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"$nor":[{"n_name":{"$eq":"ALGERIA"}},{"n_regionkey":{"$eq":1}},{"n_name":{"$in":["EGYPT","INDONESIA"]}},{"n_name":{"$not":{"$in":["CHINA","ROMANIA","SAUDI ARABIA"]}}}]} 
      -> [region_s] FETCH: plan_stability_subjoin_cardinality_md.region {"r_regionkey":{"$eq":4}} 
          -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.region r_regionkey_1
  -> [supplier] FETCH: plan_stability_subjoin_cardinality_md.supplier {"$nor":[{"s_nationkey":{"$eq":1}},{"s_nationkey":{"$eq":14}},{"s_acctbal":{"$gte":2060.13}}]} 
      -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.supplier s_nationkey_1
```
Estimated cardinality: 6  
Actual cardinality: 11  
Orders of magnitude: 1

---
### >>> Subjoin 32-3
```
db.nation.aggregate(EJSON.deserialize(
[
{"$match":{"$nor":[{"n_name":{"$eq":"ALGERIA"}},{"n_regionkey":{"$eq":1}},{"n_name":{"$in":["EGYPT","INDONESIA"]}},{"n_name":{"$not":{"$in":["CHINA","ROMANIA","SAUDI ARABIA"]}}}]}},
{"$lookup":{"from":"region","localField":"n_regionkey","foreignField":"r_regionkey","as":"region","pipeline":[
{"$match":{"$and":[{"r_regionkey":{"$eq":4}},{}]}}]}},
{"$unwind":"$region"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$region"]}}},
{"$lookup":{"from":"supplier","localField":"n_nationkey","foreignField":"s_nationkey","as":"supplier","pipeline":[
{"$match":{"$and":[{"$nor":[{"s_nationkey":{"$eq":1}},{"s_nationkey":{"$eq":14}},{"s_acctbal":{"$gte":2060.13}}]},{}]}}]}},
{"$unwind":"$supplier"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$supplier"]}}},
{"$lookup":{"from":"partsupp","localField":"s_suppkey","foreignField":"ps_suppkey","as":"partsupp","pipeline":[
{"$match":{"$and":[{"ps_comment":{"$regex":"^he"}},{}]}}]}},
{"$unwind":"$partsupp"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$partsupp"]}}}]
));
```
Subjoin plan:
```
INLJ supplier.s_suppkey = ps_suppkey
  -> [none] INLJ nation_s.n_nationkey = s_nationkey
      -> [none] INLJ n_regionkey = r_regionkey
          -> [nation_s] COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"$nor":[{"n_name":{"$eq":"ALGERIA"}},{"n_regionkey":{"$eq":1}},{"n_name":{"$in":["EGYPT","INDONESIA"]}},{"n_name":{"$not":{"$in":["CHINA","ROMANIA","SAUDI ARABIA"]}}}]} 
          -> [region_s] FETCH: plan_stability_subjoin_cardinality_md.region {"r_regionkey":{"$eq":4}} 
              -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.region r_regionkey_1
      -> [supplier] FETCH: plan_stability_subjoin_cardinality_md.supplier {"$nor":[{"s_nationkey":{"$eq":1}},{"s_nationkey":{"$eq":14}},{"s_acctbal":{"$gte":2060.13}}]} 
          -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.supplier s_nationkey_1
  -> [none] FETCH: plan_stability_subjoin_cardinality_md.partsupp {"ps_comment":{"$regex":"^he"}} 
      -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.partsupp ps_suppkey_1
```
Estimated cardinality: 8  
Actual cardinality: 11  
Orders of magnitude: 1

---
## >>> Command idx 33
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$or":[{"ps_availqty":{"$gte":2977}},{"ps_availqty":{"$eq":2039}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"lineitem","localField":"partsupp.ps_partkey","foreignField":"l_partkey","pipeline":[
{"$match":{"$nor":[{"l_commitdate":{"$lte":"1996-07-06T00:00:00.000Z"}},{"l_shipinstruct":{"$in":["DELIVER IN PERSON","COLLECT COD"]}}]}}],"as":"lineitem"}},
{"$unwind":"$lineitem"},
{"$match":{"$and":[{"lineitem.l_suppkey":824},{"p_type":{"$regex":{"$regex":"^STANDARD","$options":""}}}]}}],"cursor":{},"idx":33}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 34
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$nor":[{"ps_supplycost":{"$gt":659.13}},{"ps_availqty":{"$eq":2977}},{"ps_supplycost":{"$lte":548.54}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"supplier","localField":"partsupp.ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$and":[{"s_nationkey":14},{"s_acctbal":{"$gt":2152.23}}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$match":{"$or":[{"supplier.s_nationkey":1},{"supplier.s_acctbal":{"$gt":9524.84}}]}}],"cursor":{},"idx":34}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 35
```
{"aggregate":"supplier","pipeline":[
{"$lookup":{"from":"nation","localField":"s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$nor":[{"n_name":"JORDAN"}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$lookup":{"from":"region","localField":"nation_s.n_regionkey","foreignField":"r_regionkey","pipeline":[
{"$match":{"$nor":[{"r_name":"ASIA"}]}}],"as":"region_s"}},
{"$unwind":"$region_s"},
{"$match":{"$nor":[{"s_name":"Supplier#000000690"},{"region_s.r_name":"ASIA"}]}}],"cursor":{},"idx":35}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 36
```
{"aggregate":"partsupp","pipeline":[
{"$lookup":{"from":"supplier","localField":"ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$nor":[{"s_acctbal":{"$lte":6537.07}},{"s_acctbal":{"$gt":7174.74}}]}},
{"$match":{"$nor":[{"s_name":"Supplier#000000448"}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$and":[{"n_name":"CANADA"},{"n_regionkey":1}]}},
{"$match":{"$nor":[{"n_regionkey":0}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$lookup":{"from":"region","localField":"nation_s.n_regionkey","foreignField":"r_regionkey","pipeline":[
{"$match":{"$or":[{"r_name":"AFRICA"},{"r_name":"EUROPE"},{"r_name":"EUROPE"},{"r_regionkey":1}]}}],"as":"region_s"}},
{"$unwind":"$region_s"},
{"$match":{"$or":[{"ps_comment":{"$regex":{"$regex":"^. ","$options":""}}}]}}],"cursor":{},"idx":36}
```
### >>> Subjoin 36-0
```
db.nation.aggregate(EJSON.deserialize(
[
{"$match":{"$and":[{"n_name":{"$eq":"CANADA"}},{"n_regionkey":1}]}}]
));
```
Subjoin plan:
```
FETCH: plan_stability_subjoin_cardinality_md.nation {"n_name":{"$eq":"CANADA"}} 
  -> IXSCAN: plan_stability_subjoin_cardinality_md.nation n_regionkey_1 {"n_regionkey":["[1.0, 1.0]"]}
```
Estimated cardinality: 1  
Actual cardinality: 1  
Orders of magnitude: 0

---
### >>> Subjoin 36-1
```
db.nation.aggregate(EJSON.deserialize(
[
{"$match":{"$and":[{"n_name":{"$eq":"CANADA"}},{"n_regionkey":1}]}},
{"$lookup":{"from":"region","localField":"n_regionkey","foreignField":"r_regionkey","as":"region","pipeline":[
{"$match":{"$or":[{"r_regionkey":{"$eq":1}},{"r_name":{"$in":["AFRICA","EUROPE"]}}]}}]}},
{"$unwind":"$region"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$region"]}}}]
));
```
Subjoin plan:
```
NLJ n_regionkey = r_regionkey
  -> [nation_s] FETCH: plan_stability_subjoin_cardinality_md.nation {"n_name":{"$eq":"CANADA"}} 
      -> IXSCAN: plan_stability_subjoin_cardinality_md.nation n_regionkey_1 {"n_regionkey":["[1.0, 1.0]"]}
  -> [region_s] COLLSCAN: plan_stability_subjoin_cardinality_md.region {"$or":[{"r_regionkey":{"$eq":1}},{"r_name":{"$in":["AFRICA","EUROPE"]}}]}
```
Estimated cardinality: 1  
Actual cardinality: 1  
Orders of magnitude: 0

---
### >>> Subjoin 36-2
```
db.nation.aggregate(EJSON.deserialize(
[
{"$match":{"$and":[{"n_name":{"$eq":"CANADA"}},{"n_regionkey":1}]}},
{"$lookup":{"from":"region","localField":"n_regionkey","foreignField":"r_regionkey","as":"region","pipeline":[
{"$match":{"$or":[{"r_regionkey":{"$eq":1}},{"r_name":{"$in":["AFRICA","EUROPE"]}}]}}]}},
{"$unwind":"$region"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$region"]}}},
{"$lookup":{"from":"supplier","localField":"n_nationkey","foreignField":"s_nationkey","as":"supplier","pipeline":[
{"$match":{"$and":[{"$and":[{"s_name":{"$not":{"$eq":"Supplier#000000448"}}},{"$nor":[{"s_acctbal":{"$lte":6537.07}},{"s_acctbal":{"$gt":7174.74}}]}]},{}]}}]}},
{"$unwind":"$supplier"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$supplier"]}}}]
));
```
Subjoin plan:
```
INLJ nation_s.n_nationkey = s_nationkey
  -> [none] NLJ n_regionkey = r_regionkey
      -> [nation_s] FETCH: plan_stability_subjoin_cardinality_md.nation {"n_name":{"$eq":"CANADA"}} 
          -> IXSCAN: plan_stability_subjoin_cardinality_md.nation n_regionkey_1 {"n_regionkey":["[1.0, 1.0]"]}
      -> [region_s] COLLSCAN: plan_stability_subjoin_cardinality_md.region {"$or":[{"r_regionkey":{"$eq":1}},{"r_name":{"$in":["AFRICA","EUROPE"]}}]} 
  -> [supplier] FETCH: plan_stability_subjoin_cardinality_md.supplier {"$and":[{"s_name":{"$not":{"$eq":"Supplier#000000448"}}},{"$nor":[{"s_acctbal":{"$lte":6537.07}},{"s_acctbal":{"$gt":7174.74}}]}]} 
      -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.supplier s_nationkey_1
```
Estimated cardinality: 1  
Actual cardinality: 4  
Orders of magnitude: 0

---
### >>> Subjoin 36-3
```
db.nation.aggregate(EJSON.deserialize(
[
{"$match":{"$and":[{"n_name":{"$eq":"CANADA"}},{"n_regionkey":1}]}},
{"$lookup":{"from":"region","localField":"n_regionkey","foreignField":"r_regionkey","as":"region","pipeline":[
{"$match":{"$or":[{"r_regionkey":{"$eq":1}},{"r_name":{"$in":["AFRICA","EUROPE"]}}]}}]}},
{"$unwind":"$region"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$region"]}}},
{"$lookup":{"from":"supplier","localField":"n_nationkey","foreignField":"s_nationkey","as":"supplier","pipeline":[
{"$match":{"$and":[{"$and":[{"s_name":{"$not":{"$eq":"Supplier#000000448"}}},{"$nor":[{"s_acctbal":{"$lte":6537.07}},{"s_acctbal":{"$gt":7174.74}}]}]},{}]}}]}},
{"$unwind":"$supplier"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$supplier"]}}},
{"$lookup":{"from":"partsupp","localField":"s_suppkey","foreignField":"ps_suppkey","as":"partsupp","pipeline":[
{"$match":{"$and":[{"ps_comment":{"$regex":"^. "}},{}]}}]}},
{"$unwind":"$partsupp"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$partsupp"]}}}]
));
```
Subjoin plan:
```
INLJ supplier.s_suppkey = ps_suppkey
  -> [none] INLJ nation_s.n_nationkey = s_nationkey
      -> [none] NLJ n_regionkey = r_regionkey
          -> [nation_s] FETCH: plan_stability_subjoin_cardinality_md.nation {"n_name":{"$eq":"CANADA"}} 
              -> IXSCAN: plan_stability_subjoin_cardinality_md.nation n_regionkey_1 {"n_regionkey":["[1.0, 1.0]"]}
          -> [region_s] COLLSCAN: plan_stability_subjoin_cardinality_md.region {"$or":[{"r_regionkey":{"$eq":1}},{"r_name":{"$in":["AFRICA","EUROPE"]}}]} 
      -> [supplier] FETCH: plan_stability_subjoin_cardinality_md.supplier {"$and":[{"s_name":{"$not":{"$eq":"Supplier#000000448"}}},{"$nor":[{"s_acctbal":{"$lte":6537.07}},{"s_acctbal":{"$gt":7174.74}}]}]} 
          -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.supplier s_nationkey_1
  -> [none] FETCH: plan_stability_subjoin_cardinality_md.partsupp {"ps_comment":{"$regex":"^. "}} 
      -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.partsupp ps_suppkey_1
```
Estimated cardinality: 14  
Actual cardinality: 33  
Orders of magnitude: 0

---
## >>> Command idx 37
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$or":[{"ps_supplycost":{"$lte":889.05}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"supplier","localField":"partsupp.ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$and":[{"s_acctbal":{"$gte":1218.59}},{"s_acctbal":{"$lt":1871.86}}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$nor":[{"n_regionkey":0}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$match":{"$nor":[{"nation_s.n_name":{"$nin":["INDIA","RUSSIA"]}}]}}],"cursor":{},"idx":37}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 38
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$and":[{"ps_comment":{"$regex":{"$regex":"^nic","$options":""}}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"supplier","localField":"partsupp.ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$nor":[{"s_acctbal":{"$eq":5322.35}},{"s_acctbal":{"$lt":-128.86}},{"s_nationkey":22},{"s_acctbal":{"$gt":1033.1}}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$or":[{"n_regionkey":0}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$lookup":{"from":"region","localField":"nation_s.n_regionkey","foreignField":"r_regionkey","pipeline":[
{"$match":{"$nor":[{"r_name":"AMERICA"},{"r_name":"EUROPE"}]}},
{"$match":{"$and":[{"r_regionkey":0}]}},
{"$match":{"$or":[{"r_name":"AFRICA"}]}}],"as":"region_s"}},
{"$unwind":"$region_s"},
{"$match":{"$nor":[{"supplier.o_totalprice":{"$gt":82814.62}}]}}],"cursor":{},"idx":38}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 39
```
{"aggregate":"partsupp","pipeline":[
{"$lookup":{"from":"supplier","localField":"ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$nor":[{"s_nationkey":13},{"s_acctbal":{"$eq":7337.45}}]}},
{"$match":{"$or":[{"s_acctbal":{"$gte":1084.18}}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$or":[{"n_regionkey":1},{"n_name":{"$in":["PERU","VIETNAM"]}},{"n_name":"INDIA"}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$match":{"$or":[{"ps_comment":{"$regex":{"$regex":"^th","$options":""}}}]}}],"cursor":{},"idx":39}
```
### >>> Subjoin 39-0
```
db.nation.aggregate(EJSON.deserialize(
[
{"$match":{"$or":[{"n_name":{"$eq":"INDIA"}},{"n_regionkey":{"$eq":1}},{"n_name":{"$in":["PERU","VIETNAM"]}}]}}]
));
```
Subjoin plan:
```
COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"$or":[{"n_name":{"$eq":"INDIA"}},{"n_regionkey":{"$eq":1}},{"n_name":{"$in":["PERU","VIETNAM"]}}]}
```
Estimated cardinality: 7  
Actual cardinality: 7  
Orders of magnitude: 0

---
### >>> Subjoin 39-1
```
db.nation.aggregate(EJSON.deserialize(
[
{"$match":{"$or":[{"n_name":{"$eq":"INDIA"}},{"n_regionkey":{"$eq":1}},{"n_name":{"$in":["PERU","VIETNAM"]}}]}},
{"$lookup":{"from":"supplier","localField":"n_nationkey","foreignField":"s_nationkey","as":"supplier","pipeline":[
{"$match":{"$and":[{"s_acctbal":{"$gte":1084.18}},{"$nor":[{"s_acctbal":{"$eq":7337.45}},{"s_nationkey":{"$eq":13}}]}]}}]}},
{"$unwind":"$supplier"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$supplier"]}}}]
));
```
Subjoin plan:
```
HJ n_nationkey = s_nationkey
  -> [nation_s] COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"$or":[{"n_name":{"$eq":"INDIA"}},{"n_regionkey":{"$eq":1}},{"n_name":{"$in":["PERU","VIETNAM"]}}]} 
  -> [supplier] COLLSCAN: plan_stability_subjoin_cardinality_md.supplier {"$and":[{"s_acctbal":{"$gte":1084.18}},{"$nor":[{"s_acctbal":{"$eq":7337.45}},{"s_nationkey":{"$eq":13}}]}]}
```
Estimated cardinality: 220  
Actual cardinality: 221  
Orders of magnitude: 0

---
### >>> Subjoin 39-2
```
db.nation.aggregate(EJSON.deserialize(
[
{"$match":{"$or":[{"n_name":{"$eq":"INDIA"}},{"n_regionkey":{"$eq":1}},{"n_name":{"$in":["PERU","VIETNAM"]}}]}},
{"$lookup":{"from":"supplier","localField":"n_nationkey","foreignField":"s_nationkey","as":"supplier","pipeline":[
{"$match":{"$and":[{"s_acctbal":{"$gte":1084.18}},{"$nor":[{"s_acctbal":{"$eq":7337.45}},{"s_nationkey":{"$eq":13}}]}]}}]}},
{"$unwind":"$supplier"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$supplier"]}}},
{"$lookup":{"from":"partsupp","localField":"s_suppkey","foreignField":"ps_suppkey","as":"partsupp","pipeline":[
{"$match":{"ps_comment":{"$regex":"^th"}}}]}},
{"$unwind":"$partsupp"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$partsupp"]}}}]
));
```
Subjoin plan:
```
HJ supplier.s_suppkey = ps_suppkey
  -> [none] HJ n_nationkey = s_nationkey
      -> [nation_s] COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"$or":[{"n_name":{"$eq":"INDIA"}},{"n_regionkey":{"$eq":1}},{"n_name":{"$in":["PERU","VIETNAM"]}}]} 
      -> [supplier] COLLSCAN: plan_stability_subjoin_cardinality_md.supplier {"$and":[{"s_acctbal":{"$gte":1084.18}},{"$nor":[{"s_acctbal":{"$eq":7337.45}},{"s_nationkey":{"$eq":13}}]}]} 
  -> [none] COLLSCAN: plan_stability_subjoin_cardinality_md.partsupp {"ps_comment":{"$regex":"^th"}}
```
Estimated cardinality: 351  
Actual cardinality: 285  
Orders of magnitude: 0

---
## >>> Command idx 40
```
{"aggregate":"supplier","pipeline":[
{"$lookup":{"from":"nation","localField":"s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$or":[{"n_regionkey":1},{"n_name":"MOZAMBIQUE"}]}},
{"$match":{"$or":[{"n_regionkey":0}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$lookup":{"from":"region","localField":"nation_s.n_regionkey","foreignField":"r_regionkey","pipeline":[
{"$match":{"$and":[{"r_regionkey":0}]}}],"as":"region_s"}},
{"$unwind":"$region_s"},
{"$match":{"$or":[{"s_acctbal":{"$eq":1033.1}},{"region_s.r_name":"EUROPE"},{"region_s.r_regionkey":0}]}}],"cursor":{},"idx":40}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 41
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$nor":[{"ps_comment":{"$regex":{"$regex":"^qu","$options":""}}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"supplier","localField":"partsupp.ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$or":[{"s_name":"Supplier#000000860"},{"s_nationkey":1}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$match":{"$or":[{"p_name":"blanched sky rose chocolate royal"},{"supplier.o_orderpriority":"5-LOW"}]}}],"cursor":{},"idx":41}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 42
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$nor":[{"ps_comment":{"$regex":{"$regex":"^ bl","$options":""}}},{"ps_comment":{"$regex":{"$regex":"^are","$options":""}}}]}},
{"$match":{"$nor":[{"ps_availqty":{"$lte":5420}},{"ps_comment":{"$regex":{"$regex":"^ ","$options":""}}}]}},
{"$match":{"$or":[{"ps_comment":{"$regex":{"$regex":"^t","$options":""}}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"supplier","localField":"partsupp.ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$or":[{"s_name":"Supplier#000000085"},{"s_acctbal":{"$eq":6835.16}},{"s_acctbal":{"$gte":1218.59}}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$nor":[{"n_name":"CHINA"}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$match":{"$and":[{"supplier.s_name":"Supplier#000000540"}]}}],"cursor":{},"idx":42}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 43
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$and":[{"ps_supplycost":{"$lt":892.51}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"supplier","localField":"partsupp.ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$nor":[{"s_acctbal":{"$gt":-334.52}}]}},
{"$match":{"$or":[{"s_nationkey":3}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$match":{"$and":[{"p_size":{"$lt":8}}]}}],"cursor":{},"idx":43}
```
### >>> Subjoin 43-0
```
db.supplier.aggregate(EJSON.deserialize(
[
{"$match":{"$and":[{"s_acctbal":{"$not":{"$gt":-334.52}}},{"s_nationkey":3}]}}]
));
```
Subjoin plan:
```
FETCH: plan_stability_subjoin_cardinality_md.supplier {"s_acctbal":{"$not":{"$gt":-334.52}}} 
  -> IXSCAN: plan_stability_subjoin_cardinality_md.supplier s_nationkey_1 {"s_nationkey":["[3.0, 3.0]"]}
```
Estimated cardinality: 2  
Actual cardinality: 2  
Orders of magnitude: 0

---
### >>> Subjoin 43-1
```
db.supplier.aggregate(EJSON.deserialize(
[
{"$match":{"$and":[{"s_acctbal":{"$not":{"$gt":-334.52}}},{"s_nationkey":3}]}},
{"$lookup":{"from":"partsupp","localField":"s_suppkey","foreignField":"ps_suppkey","as":"partsupp","pipeline":[
{"$match":{"$and":[{"ps_supplycost":{"$lt":892.51}},{}]}}]}},
{"$unwind":"$partsupp"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$partsupp"]}}}]
));
```
Subjoin plan:
```
INLJ s_suppkey = ps_suppkey
  -> [supplier] FETCH: plan_stability_subjoin_cardinality_md.supplier {"s_acctbal":{"$not":{"$gt":-334.52}}} 
      -> IXSCAN: plan_stability_subjoin_cardinality_md.supplier s_nationkey_1 {"s_nationkey":["[3.0, 3.0]"]}
  -> [partsupp] FETCH: plan_stability_subjoin_cardinality_md.partsupp {"ps_supplycost":{"$lt":892.51}} 
      -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.partsupp ps_suppkey_1
```
Estimated cardinality: 144  
Actual cardinality: 138  
Orders of magnitude: 0

---
### >>> Subjoin 43-2
```
db.supplier.aggregate(EJSON.deserialize(
[
{"$match":{"$and":[{"s_acctbal":{"$not":{"$gt":-334.52}}},{"s_nationkey":3}]}},
{"$lookup":{"from":"partsupp","localField":"s_suppkey","foreignField":"ps_suppkey","as":"partsupp","pipeline":[
{"$match":{"$and":[{"ps_supplycost":{"$lt":892.51}},{}]}}]}},
{"$unwind":"$partsupp"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$partsupp"]}}},
{"$lookup":{"from":"part","localField":"ps_partkey","foreignField":"p_partkey","as":"part","pipeline":[
{"$match":{"p_size":{"$lt":8}}}]}},
{"$unwind":"$part"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$part"]}}}]
));
```
Subjoin plan:
```
HJ partsupp.ps_partkey = p_partkey
  -> [none] INLJ s_suppkey = ps_suppkey
      -> [supplier] FETCH: plan_stability_subjoin_cardinality_md.supplier {"s_acctbal":{"$not":{"$gt":-334.52}}} 
          -> IXSCAN: plan_stability_subjoin_cardinality_md.supplier s_nationkey_1 {"s_nationkey":["[3.0, 3.0]"]}
      -> [partsupp] FETCH: plan_stability_subjoin_cardinality_md.partsupp {"ps_supplycost":{"$lt":892.51}} 
          -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.partsupp ps_suppkey_1
  -> [none] COLLSCAN: plan_stability_subjoin_cardinality_md.part {"p_size":{"$lt":8}}
```
Estimated cardinality: 20  
Actual cardinality: 25  
Orders of magnitude: 0

---
## >>> Command idx 44
```
{"aggregate":"partsupp","pipeline":[
{"$lookup":{"from":"supplier","localField":"ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$or":[{"s_acctbal":{"$lt":9537.73}},{"s_name":"Supplier#000000920"}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$nor":[{"n_regionkey":2},{"n_name":"IRAN"}]}},
{"$match":{"$nor":[{"n_regionkey":4}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$match":{"$or":[{"nation_s.n_regionkey":2},{"ps_comment":{"$regex":{"$regex":"^ea","$options":""}}}]}}],"cursor":{},"idx":44}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 45
```
{"aggregate":"supplier","pipeline":[
{"$lookup":{"from":"nation","localField":"s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$and":[{"n_name":"INDIA"}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$lookup":{"from":"region","localField":"nation_s.n_regionkey","foreignField":"r_regionkey","pipeline":[
{"$match":{"$or":[{"r_name":"AFRICA"},{"r_name":"ASIA"}]}}],"as":"region_s"}},
{"$unwind":"$region_s"},
{"$match":{"$nor":[{"region_s.r_regionkey":0}]}}],"cursor":{},"idx":45}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 46
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$nor":[{"ps_availqty":{"$gte":9516}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"supplier","localField":"partsupp.ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$nor":[{"s_name":"Supplier#000001000"},{"s_acctbal":{"$gt":-686.97}}]}},
{"$match":{"$nor":[{"s_acctbal":{"$eq":6537.07}}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$or":[{"n_name":"GERMANY"},{"n_regionkey":0}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$match":{"$or":[{"partsupp.ps_availqty":{"$lt":7831}},{"supplier.o_orderdate":{"$lte":"1996-12-28T00:00:00.000Z"}},{"supplier.o_orderpriority":"5-LOW"}]}}],"cursor":{},"idx":46}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 47
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$nor":[{"ps_availqty":{"$lt":4947}},{"ps_supplycost":{"$lt":13.2}},{"ps_supplycost":{"$lte":528.21}}]}},
{"$match":{"$nor":[{"ps_availqty":{"$lte":8052}},{"ps_comment":{"$regex":{"$regex":"^fina","$options":""}}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"supplier","localField":"partsupp.ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$nor":[{"s_acctbal":{"$eq":7241.4}},{"s_nationkey":13},{"s_acctbal":{"$lte":5302.37}}]}},
{"$match":{"$or":[{"s_nationkey":22}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$nor":[{"n_name":"CHINA"}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$lookup":{"from":"region","localField":"nation_s.n_regionkey","foreignField":"r_regionkey","pipeline":[
{"$match":{"$or":[{"r_regionkey":3},{"r_name":"EUROPE"}]}},
{"$match":{"$or":[{"r_name":"EUROPE"},{"r_regionkey":3}]}},
{"$match":{"$nor":[{"r_name":"AFRICA"}]}}],"as":"region_s"}},
{"$unwind":"$region_s"},
{"$match":{"$and":[{"p_retailprice":{"$lt":1102.18}}]}}],"cursor":{},"idx":47}
```
### >>> Subjoin 47-0
```
db.region.aggregate(EJSON.deserialize(
[
{"$match":{"$and":[{"$or":[{"r_name":{"$eq":"EUROPE"}},{"r_regionkey":{"$eq":3}}]},{"$or":[{"r_name":{"$eq":"EUROPE"}},{"r_regionkey":{"$eq":3}}]},{"r_name":{"$not":{"$eq":"AFRICA"}}}]}}]
));
```
Subjoin plan:
```
COLLSCAN: plan_stability_subjoin_cardinality_md.region {"$and":[{"$or":[{"r_name":{"$eq":"EUROPE"}},{"r_regionkey":{"$eq":3}}]},{"$or":[{"r_name":{"$eq":"EUROPE"}},{"r_regionkey":{"$eq":3}}]},{"r_name":{"$not":{"$eq":"AFRICA"}}}]}
```
Estimated cardinality: 1  
Actual cardinality: 1  
Orders of magnitude: 0

---
### >>> Subjoin 47-1
```
db.region.aggregate(EJSON.deserialize(
[
{"$match":{"$and":[{"$or":[{"r_name":{"$eq":"EUROPE"}},{"r_regionkey":{"$eq":3}}]},{"$or":[{"r_name":{"$eq":"EUROPE"}},{"r_regionkey":{"$eq":3}}]},{"r_name":{"$not":{"$eq":"AFRICA"}}}]}},
{"$lookup":{"from":"nation","localField":"r_regionkey","foreignField":"n_regionkey","as":"nation","pipeline":[
{"$match":{"n_name":{"$not":{"$eq":"CHINA"}}}}]}},
{"$unwind":"$nation"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$nation"]}}}]
));
```
Subjoin plan:
```
NLJ r_regionkey = n_regionkey
  -> [region_s] COLLSCAN: plan_stability_subjoin_cardinality_md.region {"$and":[{"$or":[{"r_name":{"$eq":"EUROPE"}},{"r_regionkey":{"$eq":3}}]},{"$or":[{"r_name":{"$eq":"EUROPE"}},{"r_regionkey":{"$eq":3}}]},{"r_name":{"$not":{"$eq":"AFRICA"}}}]} 
  -> [nation_s] COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"n_name":{"$not":{"$eq":"CHINA"}}}
```
Estimated cardinality: 5  
Actual cardinality: 5  
Orders of magnitude: 0

---
### >>> Subjoin 47-2
```
db.region.aggregate(EJSON.deserialize(
[
{"$match":{"$and":[{"$or":[{"r_name":{"$eq":"EUROPE"}},{"r_regionkey":{"$eq":3}}]},{"$or":[{"r_name":{"$eq":"EUROPE"}},{"r_regionkey":{"$eq":3}}]},{"r_name":{"$not":{"$eq":"AFRICA"}}}]}},
{"$lookup":{"from":"nation","localField":"r_regionkey","foreignField":"n_regionkey","as":"nation","pipeline":[
{"$match":{"n_name":{"$not":{"$eq":"CHINA"}}}}]}},
{"$unwind":"$nation"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$nation"]}}},
{"$lookup":{"from":"supplier","localField":"n_nationkey","foreignField":"s_nationkey","as":"supplier","pipeline":[
{"$match":{"$and":[{"$nor":[{"s_acctbal":{"$eq":7241.4}},{"s_acctbal":{"$lte":5302.37}},{"s_nationkey":{"$eq":13}}]},{"s_nationkey":22}]}}]}},
{"$unwind":"$supplier"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$supplier"]}}}]
));
```
Subjoin plan:
```
HJ nation_s.n_nationkey = s_nationkey
  -> [none] NLJ r_regionkey = n_regionkey
      -> [region_s] COLLSCAN: plan_stability_subjoin_cardinality_md.region {"$and":[{"$or":[{"r_name":{"$eq":"EUROPE"}},{"r_regionkey":{"$eq":3}}]},{"$or":[{"r_name":{"$eq":"EUROPE"}},{"r_regionkey":{"$eq":3}}]},{"r_name":{"$not":{"$eq":"AFRICA"}}}]} 
      -> [nation_s] COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"n_name":{"$not":{"$eq":"CHINA"}}} 
  -> [supplier] FETCH: plan_stability_subjoin_cardinality_md.supplier {"$nor":[{"s_acctbal":{"$eq":7241.4}},{"s_acctbal":{"$lte":5302.37}},{"s_nationkey":{"$eq":13}}]} 
      -> IXSCAN: plan_stability_subjoin_cardinality_md.supplier s_nationkey_1 {"s_nationkey":["[22.0, 22.0]"]}
```
Estimated cardinality: 4  
Actual cardinality: 22  
Orders of magnitude: 1

---
### >>> Subjoin 47-3
```
db.region.aggregate(EJSON.deserialize(
[
{"$match":{"$and":[{"$or":[{"r_name":{"$eq":"EUROPE"}},{"r_regionkey":{"$eq":3}}]},{"$or":[{"r_name":{"$eq":"EUROPE"}},{"r_regionkey":{"$eq":3}}]},{"r_name":{"$not":{"$eq":"AFRICA"}}}]}},
{"$lookup":{"from":"nation","localField":"r_regionkey","foreignField":"n_regionkey","as":"nation","pipeline":[
{"$match":{"n_name":{"$not":{"$eq":"CHINA"}}}}]}},
{"$unwind":"$nation"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$nation"]}}},
{"$lookup":{"from":"supplier","localField":"n_nationkey","foreignField":"s_nationkey","as":"supplier","pipeline":[
{"$match":{"$and":[{"$nor":[{"s_acctbal":{"$eq":7241.4}},{"s_acctbal":{"$lte":5302.37}},{"s_nationkey":{"$eq":13}}]},{"s_nationkey":22}]}}]}},
{"$unwind":"$supplier"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$supplier"]}}},
{"$lookup":{"from":"partsupp","localField":"s_suppkey","foreignField":"ps_suppkey","as":"partsupp","pipeline":[
{"$match":{"$and":[{"$and":[{"$nor":[{"ps_availqty":{"$lte":8052}},{"ps_comment":{"$regex":"^fina"}}]},{"$nor":[{"ps_supplycost":{"$lte":528.21}},{"ps_availqty":{"$lt":4947}},{"ps_supplycost":{"$lt":13.2}}]}]},{}]}}]}},
{"$unwind":"$partsupp"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$partsupp"]}}}]
));
```
Subjoin plan:
```
INLJ supplier.s_suppkey = ps_suppkey
  -> [none] HJ nation_s.n_nationkey = s_nationkey
      -> [none] NLJ r_regionkey = n_regionkey
          -> [region_s] COLLSCAN: plan_stability_subjoin_cardinality_md.region {"$and":[{"$or":[{"r_name":{"$eq":"EUROPE"}},{"r_regionkey":{"$eq":3}}]},{"$or":[{"r_name":{"$eq":"EUROPE"}},{"r_regionkey":{"$eq":3}}]},{"r_name":{"$not":{"$eq":"AFRICA"}}}]} 
          -> [nation_s] COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"n_name":{"$not":{"$eq":"CHINA"}}} 
      -> [supplier] FETCH: plan_stability_subjoin_cardinality_md.supplier {"$nor":[{"s_acctbal":{"$eq":7241.4}},{"s_acctbal":{"$lte":5302.37}},{"s_nationkey":{"$eq":13}}]} 
          -> IXSCAN: plan_stability_subjoin_cardinality_md.supplier s_nationkey_1 {"s_nationkey":["[22.0, 22.0]"]}
  -> [partsupp] FETCH: plan_stability_subjoin_cardinality_md.partsupp {"$and":[{"$nor":[{"ps_availqty":{"$lte":8052}},{"ps_comment":{"$regex":"^fina"}}]},{"$nor":[{"ps_supplycost":{"$lte":528.21}},{"ps_availqty":{"$lt":4947}},{"ps_supplycost":{"$lt":13.2}}]}]} 
      -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.partsupp ps_suppkey_1
```
Estimated cardinality: 27  
Actual cardinality: 154  
Orders of magnitude: 1

---
### >>> Subjoin 47-4
```
db.region.aggregate(EJSON.deserialize(
[
{"$match":{"$and":[{"$or":[{"r_name":{"$eq":"EUROPE"}},{"r_regionkey":{"$eq":3}}]},{"$or":[{"r_name":{"$eq":"EUROPE"}},{"r_regionkey":{"$eq":3}}]},{"r_name":{"$not":{"$eq":"AFRICA"}}}]}},
{"$lookup":{"from":"nation","localField":"r_regionkey","foreignField":"n_regionkey","as":"nation","pipeline":[
{"$match":{"n_name":{"$not":{"$eq":"CHINA"}}}}]}},
{"$unwind":"$nation"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$nation"]}}},
{"$lookup":{"from":"supplier","localField":"n_nationkey","foreignField":"s_nationkey","as":"supplier","pipeline":[
{"$match":{"$and":[{"$nor":[{"s_acctbal":{"$eq":7241.4}},{"s_acctbal":{"$lte":5302.37}},{"s_nationkey":{"$eq":13}}]},{"s_nationkey":22}]}}]}},
{"$unwind":"$supplier"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$supplier"]}}},
{"$lookup":{"from":"partsupp","localField":"s_suppkey","foreignField":"ps_suppkey","as":"partsupp","pipeline":[
{"$match":{"$and":[{"$and":[{"$nor":[{"ps_availqty":{"$lte":8052}},{"ps_comment":{"$regex":"^fina"}}]},{"$nor":[{"ps_supplycost":{"$lte":528.21}},{"ps_availqty":{"$lt":4947}},{"ps_supplycost":{"$lt":13.2}}]}]},{}]}}]}},
{"$unwind":"$partsupp"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$partsupp"]}}},
{"$lookup":{"from":"part","localField":"ps_partkey","foreignField":"p_partkey","as":"part","pipeline":[
{"$match":{"$and":[{"p_retailprice":{"$lt":1102.18}},{}]}}]}},
{"$unwind":"$part"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$part"]}}}]
));
```
Subjoin plan:
```
INLJ partsupp.ps_partkey = p_partkey
  -> [none] INLJ supplier.s_suppkey = ps_suppkey
      -> [none] HJ nation_s.n_nationkey = s_nationkey
          -> [none] NLJ r_regionkey = n_regionkey
              -> [region_s] COLLSCAN: plan_stability_subjoin_cardinality_md.region {"$and":[{"$or":[{"r_name":{"$eq":"EUROPE"}},{"r_regionkey":{"$eq":3}}]},{"$or":[{"r_name":{"$eq":"EUROPE"}},{"r_regionkey":{"$eq":3}}]},{"r_name":{"$not":{"$eq":"AFRICA"}}}]} 
              -> [nation_s] COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"n_name":{"$not":{"$eq":"CHINA"}}} 
          -> [supplier] FETCH: plan_stability_subjoin_cardinality_md.supplier {"$nor":[{"s_acctbal":{"$eq":7241.4}},{"s_acctbal":{"$lte":5302.37}},{"s_nationkey":{"$eq":13}}]} 
              -> IXSCAN: plan_stability_subjoin_cardinality_md.supplier s_nationkey_1 {"s_nationkey":["[22.0, 22.0]"]}
      -> [partsupp] FETCH: plan_stability_subjoin_cardinality_md.partsupp {"$and":[{"$nor":[{"ps_availqty":{"$lte":8052}},{"ps_comment":{"$regex":"^fina"}}]},{"$nor":[{"ps_supplycost":{"$lte":528.21}},{"ps_availqty":{"$lt":4947}},{"ps_supplycost":{"$lt":13.2}}]}]} 
          -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.partsupp ps_suppkey_1
  -> [none] FETCH: plan_stability_subjoin_cardinality_md.part {"p_retailprice":{"$lt":1102.18}} 
      -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.part p_partkey_1
```
Estimated cardinality: 5  
Actual cardinality: 39  
Orders of magnitude: 1

---
## >>> Command idx 48
```
{"aggregate":"partsupp","pipeline":[
{"$lookup":{"from":"supplier","localField":"ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$or":[{"s_nationkey":12},{"s_acctbal":{"$lte":5046.81}}]}},
{"$match":{"$nor":[{"s_nationkey":7},{"s_name":"Supplier#000000836"},{"s_name":"Supplier#000000920"}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$and":[{"n_regionkey":2}]}},
{"$match":{"$or":[{"n_name":{"$in":["INDONESIA","KENYA","INDIA"]}}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$lookup":{"from":"region","localField":"nation_s.n_regionkey","foreignField":"r_regionkey","pipeline":[
{"$match":{"$or":[{"r_regionkey":2}]}}],"as":"region_s"}},
{"$unwind":"$region_s"},
{"$match":{"$and":[{"ps_comment":{"$regex":{"$regex":"^y ac","$options":""}}},{"region_s.r_name":"ASIA"},{"region_s.r_name":"ASIA"}]}}],"cursor":{},"idx":48}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 49
```
{"aggregate":"partsupp","pipeline":[
{"$lookup":{"from":"supplier","localField":"ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$nor":[{"s_nationkey":2},{"s_acctbal":{"$eq":167.56}},{"s_nationkey":6}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$nor":[{"n_regionkey":0}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$lookup":{"from":"region","localField":"nation_s.n_regionkey","foreignField":"r_regionkey","pipeline":[
{"$match":{"$or":[{"r_name":"AMERICA"}]}},
{"$match":{"$or":[{"r_name":"MIDDLE EAST"},{"r_regionkey":1}]}}],"as":"region_s"}},
{"$unwind":"$region_s"},
{"$match":{"$or":[{"ps_supplycost":{"$eq":947.21}},{"nation_s.n_regionkey":4}]}}],"cursor":{},"idx":49}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 50
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$and":[{"ps_availqty":{"$gt":9410}},{"ps_supplycost":{"$lte":587.19}}]}},
{"$match":{"$or":[{"ps_supplycost":{"$lte":804.73}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"supplier","localField":"partsupp.ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$or":[{"s_acctbal":{"$lte":6741.18}}]}},
{"$match":{"$nor":[{"s_acctbal":{"$lt":4327.86}}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$nor":[{"n_regionkey":2},{"n_name":{"$nin":["VIETNAM","EGYPT","MOZAMBIQUE"]}}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$lookup":{"from":"region","localField":"nation_s.n_regionkey","foreignField":"r_regionkey","pipeline":[
{"$match":{"$nor":[{"r_regionkey":2},{"r_name":"AMERICA"},{"r_regionkey":1}]}}],"as":"region_s"}},
{"$unwind":"$region_s"},
{"$match":{"$or":[{"supplier.o_orderdate":{"$eq":"1993-07-28T00:00:00.000Z"}},{"partsupp.ps_comment":{"$regex":{"$regex":"^e","$options":""}}},{"supplier.s_acctbal":{"$lt":9747.16}}]}}],"cursor":{},"idx":50}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 51
```
{"aggregate":"lineitem","pipeline":[
{"$lookup":{"from":"orders","localField":"l_orderkey","foreignField":"o_orderkey","pipeline":[
{"$match":{"$and":[{"o_orderpriority":"4-NOT SPECIFIED"},{"o_orderstatus":"O"}]}}],"as":"orders"}},
{"$unwind":"$orders"},
{"$lookup":{"from":"customer","localField":"orders.o_custkey","foreignField":"c_custkey","pipeline":[
{"$match":{"$or":[{"c_mktsegment":"HOUSEHOLD"},{"c_mktsegment":"BUILDING"},{"c_acctbal":{"$lt":108.03}}]}},
{"$match":{"$nor":[{"c_mktsegment":"HOUSEHOLD"},{"c_mktsegment":"BUILDING"}]}},
{"$match":{"$nor":[{"c_name":"Customer#000013077"}]}}],"as":"customer"}},
{"$unwind":"$customer"},
{"$lookup":{"from":"supplier","localField":"customer.c_nationkey","foreignField":"s_nationkey","pipeline":[
{"$match":{"$and":[{"s_name":"Supplier#000000569"}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$match":{"$or":[{"l_discount":{"$lte":0}}]}}],"cursor":{},"idx":51}
```
### >>> Subjoin 51-0
```
db.supplier.aggregate(EJSON.deserialize(
[
{"$match":{"s_name":{"$eq":"Supplier#000000569"}}}]
));
```
Subjoin plan:
```
COLLSCAN: plan_stability_subjoin_cardinality_md.supplier {"s_name":{"$eq":"Supplier#000000569"}}
```
Estimated cardinality: 1  
Actual cardinality: 1  
Orders of magnitude: 0

---
### >>> Subjoin 51-1
```
db.supplier.aggregate(EJSON.deserialize(
[
{"$match":{"s_name":{"$eq":"Supplier#000000569"}}},
{"$lookup":{"from":"customer","localField":"s_nationkey","foreignField":"c_nationkey","as":"customer","pipeline":[
{"$match":{"$and":[{"$and":[{"$or":[{"c_acctbal":{"$lt":108.03}},{"c_mktsegment":{"$in":["BUILDING","HOUSEHOLD"]}}]},{"c_name":{"$not":{"$eq":"Customer#000013077"}}},{"$nor":[{"c_mktsegment":{"$eq":"HOUSEHOLD"}},{"c_mktsegment":{"$eq":"BUILDING"}}]}]},{}]}}]}},
{"$unwind":"$customer"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$customer"]}}}]
));
```
Subjoin plan:
```
INLJ s_nationkey = c_nationkey
  -> [supplier] COLLSCAN: plan_stability_subjoin_cardinality_md.supplier {"s_name":{"$eq":"Supplier#000000569"}} 
  -> [customer] FETCH: plan_stability_subjoin_cardinality_md.customer {"$and":[{"$or":[{"c_acctbal":{"$lt":108.03}},{"c_mktsegment":{"$in":["BUILDING","HOUSEHOLD"]}}]},{"c_name":{"$not":{"$eq":"Customer#000013077"}}},{"$nor":[{"c_mktsegment":{"$eq":"HOUSEHOLD"}},{"c_mktsegment":{"$eq":"BUILDING"}}]}]} 
      -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.customer c_nationkey_1
```
Estimated cardinality: 33  
Actual cardinality: 40  
Orders of magnitude: 0

---
### >>> Subjoin 51-2
```
db.supplier.aggregate(EJSON.deserialize(
[
{"$match":{"s_name":{"$eq":"Supplier#000000569"}}},
{"$lookup":{"from":"customer","localField":"s_nationkey","foreignField":"c_nationkey","as":"customer","pipeline":[
{"$match":{"$and":[{"$and":[{"$or":[{"c_acctbal":{"$lt":108.03}},{"c_mktsegment":{"$in":["BUILDING","HOUSEHOLD"]}}]},{"c_name":{"$not":{"$eq":"Customer#000013077"}}},{"$nor":[{"c_mktsegment":{"$eq":"HOUSEHOLD"}},{"c_mktsegment":{"$eq":"BUILDING"}}]}]},{}]}}]}},
{"$unwind":"$customer"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$customer"]}}},
{"$lookup":{"from":"orders","localField":"c_custkey","foreignField":"o_custkey","as":"orders","pipeline":[
{"$match":{"$and":[{"$and":[{"o_orderpriority":{"$eq":"4-NOT SPECIFIED"}},{"o_orderstatus":{"$eq":"O"}}]},{}]}}]}},
{"$unwind":"$orders"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$orders"]}}}]
));
```
Subjoin plan:
```
INLJ customer.c_custkey = o_custkey
  -> [none] INLJ s_nationkey = c_nationkey
      -> [supplier] COLLSCAN: plan_stability_subjoin_cardinality_md.supplier {"s_name":{"$eq":"Supplier#000000569"}} 
      -> [customer] FETCH: plan_stability_subjoin_cardinality_md.customer {"$and":[{"$or":[{"c_acctbal":{"$lt":108.03}},{"c_mktsegment":{"$in":["BUILDING","HOUSEHOLD"]}}]},{"c_name":{"$not":{"$eq":"Customer#000013077"}}},{"$nor":[{"c_mktsegment":{"$eq":"HOUSEHOLD"}},{"c_mktsegment":{"$eq":"BUILDING"}}]}]} 
          -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.customer c_nationkey_1
  -> [orders] FETCH: plan_stability_subjoin_cardinality_md.orders {"$and":[{"o_orderpriority":{"$eq":"4-NOT SPECIFIED"}},{"o_orderstatus":{"$eq":"O"}}]} 
      -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.orders o_custkey_1
```
Estimated cardinality: 32  
Actual cardinality: 39  
Orders of magnitude: 0

---
### >>> Subjoin 51-3
```
db.supplier.aggregate(EJSON.deserialize(
[
{"$match":{"s_name":{"$eq":"Supplier#000000569"}}},
{"$lookup":{"from":"customer","localField":"s_nationkey","foreignField":"c_nationkey","as":"customer","pipeline":[
{"$match":{"$and":[{"$and":[{"$or":[{"c_acctbal":{"$lt":108.03}},{"c_mktsegment":{"$in":["BUILDING","HOUSEHOLD"]}}]},{"c_name":{"$not":{"$eq":"Customer#000013077"}}},{"$nor":[{"c_mktsegment":{"$eq":"HOUSEHOLD"}},{"c_mktsegment":{"$eq":"BUILDING"}}]}]},{}]}}]}},
{"$unwind":"$customer"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$customer"]}}},
{"$lookup":{"from":"orders","localField":"c_custkey","foreignField":"o_custkey","as":"orders","pipeline":[
{"$match":{"$and":[{"$and":[{"o_orderpriority":{"$eq":"4-NOT SPECIFIED"}},{"o_orderstatus":{"$eq":"O"}}]},{}]}}]}},
{"$unwind":"$orders"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$orders"]}}},
{"$lookup":{"from":"lineitem","localField":"o_orderkey","foreignField":"l_orderkey","as":"lineitem","pipeline":[
{"$match":{"$and":[{"l_discount":{"$lte":0}},{}]}}]}},
{"$unwind":"$lineitem"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$lineitem"]}}}]
));
```
Subjoin plan:
```
INLJ orders.o_orderkey = l_orderkey
  -> [none] INLJ customer.c_custkey = o_custkey
      -> [none] INLJ s_nationkey = c_nationkey
          -> [supplier] COLLSCAN: plan_stability_subjoin_cardinality_md.supplier {"s_name":{"$eq":"Supplier#000000569"}} 
          -> [customer] FETCH: plan_stability_subjoin_cardinality_md.customer {"$and":[{"$or":[{"c_acctbal":{"$lt":108.03}},{"c_mktsegment":{"$in":["BUILDING","HOUSEHOLD"]}}]},{"c_name":{"$not":{"$eq":"Customer#000013077"}}},{"$nor":[{"c_mktsegment":{"$eq":"HOUSEHOLD"}},{"c_mktsegment":{"$eq":"BUILDING"}}]}]} 
              -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.customer c_nationkey_1
      -> [orders] FETCH: plan_stability_subjoin_cardinality_md.orders {"$and":[{"o_orderpriority":{"$eq":"4-NOT SPECIFIED"}},{"o_orderstatus":{"$eq":"O"}}]} 
          -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.orders o_custkey_1
  -> [none] FETCH: plan_stability_subjoin_cardinality_md.lineitem {"l_discount":{"$lte":0}} 
      -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.lineitem l_orderkey_1
```
Estimated cardinality: 12  
Actual cardinality: 14  
Orders of magnitude: 0

---
## >>> Command idx 52
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$or":[{"ps_comment":{"$regex":{"$regex":"^s a","$options":""}}},{"ps_supplycost":{"$gt":47.97}},{"ps_supplycost":{"$gte":632.83}}]}},
{"$match":{"$and":[{"ps_availqty":{"$gt":298}},{"ps_comment":{"$regex":{"$regex":"^b","$options":""}}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"supplier","localField":"partsupp.ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$and":[{"s_acctbal":{"$lte":9166.95}}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$nor":[{"n_name":{"$nin":["ALGERIA","ETHIOPIA"]}}]}},
{"$match":{"$nor":[{"n_name":"INDONESIA"}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$lookup":{"from":"region","localField":"nation_s.n_regionkey","foreignField":"r_regionkey","pipeline":[
{"$match":{"$nor":[{"r_regionkey":1}]}}],"as":"region_s"}},
{"$unwind":"$region_s"},
{"$match":{"$nor":[{"supplier.o_orderpriority":"1-URGENT"}]}}],"cursor":{},"idx":52}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 53
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$and":[{"ps_availqty":{"$lt":6104}},{"ps_supplycost":{"$lte":407.57}}]}},
{"$match":{"$nor":[{"ps_availqty":{"$lt":4947}},{"ps_availqty":{"$lte":4909}},{"ps_availqty":{"$eq":7813}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"lineitem","localField":"partsupp.ps_partkey","foreignField":"l_partkey","pipeline":[
{"$match":{"$nor":[{"l_tax":{"$gt":0}}]}},
{"$match":{"$and":[{"l_suppkey":277}]}}],"as":"lineitem"}},
{"$unwind":"$lineitem"},
{"$match":{"$nor":[{"lineitem.l_suppkey":2},{"p_type":{"$regex":{"$regex":"^MEDIUM","$options":""}}}]}}],"cursor":{},"idx":53}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 54
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$nor":[{"ps_comment":{"$regex":{"$regex":"^gula","$options":""}}},{"ps_supplycost":{"$gt":528.21}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"supplier","localField":"partsupp.ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$or":[{"s_name":"Supplier#000000643"},{"s_acctbal":{"$lt":-942.73}},{"s_name":"Supplier#000000543"}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$or":[{"n_regionkey":4},{"n_regionkey":1}]}},
{"$match":{"$nor":[{"n_name":{"$in":["BRAZIL","BRAZIL","ALGERIA"]}}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$match":{"$nor":[{"p_container":{"$nin":["LG PACK","WRAP BOX"]}}]}}],"cursor":{},"idx":54}
```
### >>> Subjoin 54-0
```
db.supplier.aggregate(EJSON.deserialize(
[
{"$match":{"$or":[{"s_acctbal":{"$lt":-942.73}},{"s_name":{"$in":["Supplier#000000543","Supplier#000000643"]}}]}}]
));
```
Subjoin plan:
```
COLLSCAN: plan_stability_subjoin_cardinality_md.supplier {"$or":[{"s_acctbal":{"$lt":-942.73}},{"s_name":{"$in":["Supplier#000000543","Supplier#000000643"]}}]}
```
Estimated cardinality: 6  
Actual cardinality: 6  
Orders of magnitude: 0

---
### >>> Subjoin 54-1
```
db.supplier.aggregate(EJSON.deserialize(
[
{"$match":{"$or":[{"s_acctbal":{"$lt":-942.73}},{"s_name":{"$in":["Supplier#000000543","Supplier#000000643"]}}]}},
{"$lookup":{"from":"nation","localField":"s_nationkey","foreignField":"n_nationkey","as":"nation","pipeline":[
{"$match":{"$and":[{"n_regionkey":{"$in":[1,4]}},{"n_name":{"$not":{"$in":["ALGERIA","BRAZIL"]}}}]}}]}},
{"$unwind":"$nation"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$nation"]}}}]
));
```
Subjoin plan:
```
HJ s_nationkey = n_nationkey
  -> [supplier] COLLSCAN: plan_stability_subjoin_cardinality_md.supplier {"$or":[{"s_acctbal":{"$lt":-942.73}},{"s_name":{"$in":["Supplier#000000543","Supplier#000000643"]}}]} 
  -> [nation_s] COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"$and":[{"n_regionkey":{"$in":[1,4]}},{"n_name":{"$not":{"$in":["ALGERIA","BRAZIL"]}}}]}
```
Estimated cardinality: 2  
Actual cardinality: 3  
Orders of magnitude: 0

---
### >>> Subjoin 54-2
```
db.supplier.aggregate(EJSON.deserialize(
[
{"$match":{"$or":[{"s_acctbal":{"$lt":-942.73}},{"s_name":{"$in":["Supplier#000000543","Supplier#000000643"]}}]}},
{"$lookup":{"from":"nation","localField":"s_nationkey","foreignField":"n_nationkey","as":"nation","pipeline":[
{"$match":{"$and":[{"n_regionkey":{"$in":[1,4]}},{"n_name":{"$not":{"$in":["ALGERIA","BRAZIL"]}}}]}}]}},
{"$unwind":"$nation"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$nation"]}}},
{"$lookup":{"from":"partsupp","localField":"s_suppkey","foreignField":"ps_suppkey","as":"partsupp","pipeline":[
{"$match":{"$and":[{"$nor":[{"ps_supplycost":{"$gt":528.21}},{"ps_comment":{"$regex":"^gula"}}]},{}]}}]}},
{"$unwind":"$partsupp"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$partsupp"]}}}]
));
```
Subjoin plan:
```
INLJ supplier.s_suppkey = ps_suppkey
  -> [none] HJ s_nationkey = n_nationkey
      -> [supplier] COLLSCAN: plan_stability_subjoin_cardinality_md.supplier {"$or":[{"s_acctbal":{"$lt":-942.73}},{"s_name":{"$in":["Supplier#000000543","Supplier#000000643"]}}]} 
      -> [nation_s] COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"$and":[{"n_regionkey":{"$in":[1,4]}},{"n_name":{"$not":{"$in":["ALGERIA","BRAZIL"]}}}]} 
  -> [partsupp] FETCH: plan_stability_subjoin_cardinality_md.partsupp {"$nor":[{"ps_supplycost":{"$gt":528.21}},{"ps_comment":{"$regex":"^gula"}}]} 
      -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.partsupp ps_suppkey_1
```
Estimated cardinality: 91  
Actual cardinality: 132  
Orders of magnitude: 1

---
### >>> Subjoin 54-3
```
db.supplier.aggregate(EJSON.deserialize(
[
{"$match":{"$or":[{"s_acctbal":{"$lt":-942.73}},{"s_name":{"$in":["Supplier#000000543","Supplier#000000643"]}}]}},
{"$lookup":{"from":"nation","localField":"s_nationkey","foreignField":"n_nationkey","as":"nation","pipeline":[
{"$match":{"$and":[{"n_regionkey":{"$in":[1,4]}},{"n_name":{"$not":{"$in":["ALGERIA","BRAZIL"]}}}]}}]}},
{"$unwind":"$nation"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$nation"]}}},
{"$lookup":{"from":"partsupp","localField":"s_suppkey","foreignField":"ps_suppkey","as":"partsupp","pipeline":[
{"$match":{"$and":[{"$nor":[{"ps_supplycost":{"$gt":528.21}},{"ps_comment":{"$regex":"^gula"}}]},{}]}}]}},
{"$unwind":"$partsupp"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$partsupp"]}}},
{"$lookup":{"from":"part","localField":"ps_partkey","foreignField":"p_partkey","as":"part","pipeline":[
{"$match":{"$nor":[{"p_container":{"$not":{"$in":["LG PACK","WRAP BOX"]}}}]}}]}},
{"$unwind":"$part"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$part"]}}}]
));
```
Subjoin plan:
```
HJ partsupp.ps_partkey = p_partkey
  -> [none] INLJ supplier.s_suppkey = ps_suppkey
      -> [none] HJ s_nationkey = n_nationkey
          -> [supplier] COLLSCAN: plan_stability_subjoin_cardinality_md.supplier {"$or":[{"s_acctbal":{"$lt":-942.73}},{"s_name":{"$in":["Supplier#000000543","Supplier#000000643"]}}]} 
          -> [nation_s] COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"$and":[{"n_regionkey":{"$in":[1,4]}},{"n_name":{"$not":{"$in":["ALGERIA","BRAZIL"]}}}]} 
      -> [partsupp] FETCH: plan_stability_subjoin_cardinality_md.partsupp {"$nor":[{"ps_supplycost":{"$gt":528.21}},{"ps_comment":{"$regex":"^gula"}}]} 
          -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.partsupp ps_suppkey_1
  -> [none] COLLSCAN: plan_stability_subjoin_cardinality_md.part {"$nor":[{"p_container":{"$not":{"$in":["LG PACK","WRAP BOX"]}}}]}
```
Estimated cardinality: 4  
Actual cardinality: 5  
Orders of magnitude: 0

---
## >>> Command idx 55
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$and":[{"ps_availqty":{"$lt":9935}},{"ps_comment":{"$regex":{"$regex":"^i","$options":""}}}]}},
{"$match":{"$or":[{"ps_availqty":{"$lte":9607}},{"ps_availqty":{"$gt":5937}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"lineitem","localField":"partsupp.ps_partkey","foreignField":"l_partkey","pipeline":[
{"$match":{"$or":[{"l_tax":{"$gt":0}}]}},
{"$match":{"$and":[{"l_shipdate":{"$lte":"1996-09-22T00:00:00.000Z"}},{"l_quantity":{"$lte":26}}]}},
{"$match":{"$and":[{"l_suppkey":90},{"l_linestatus":{"$in":["F"]}}]}}],"as":"lineitem"}},
{"$unwind":"$lineitem"},
{"$match":{"$or":[{"partsupp.ps_availqty":{"$lte":4424}},{"p_mfgr":"Manufacturer#4"},{"partsupp.ps_supplycost":{"$eq":164.22}}]}}],"cursor":{},"idx":55}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 56
```
{"aggregate":"partsupp","pipeline":[
{"$lookup":{"from":"supplier","localField":"ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$or":[{"s_acctbal":{"$eq":-111.84}},{"s_acctbal":{"$gte":8562.82}}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$nor":[{"n_name":"CHINA"}]}},
{"$match":{"$or":[{"n_name":"EGYPT"}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$match":{"$nor":[{"nation_s.n_regionkey":3}]}}],"cursor":{},"idx":56}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 57
```
{"aggregate":"partsupp","pipeline":[
{"$lookup":{"from":"supplier","localField":"ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$or":[{"s_acctbal":{"$gt":9747.16}},{"s_nationkey":15},{"s_name":"Supplier#000000517"},{"s_name":"Supplier#000000439"}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$nor":[{"n_name":"MOROCCO"}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$lookup":{"from":"region","localField":"nation_s.n_regionkey","foreignField":"r_regionkey","pipeline":[
{"$match":{"$nor":[{"r_regionkey":1}]}}],"as":"region_s"}},
{"$unwind":"$region_s"},
{"$match":{"$or":[{"ps_comment":{"$regex":{"$regex":"^in","$options":""}}}]}}],"cursor":{},"idx":57}
```
### >>> Subjoin 57-0
```
db.region.aggregate(EJSON.deserialize(
[
{"$match":{"r_regionkey":{"$not":{"$eq":1}}}}]
));
```
Subjoin plan:
```
COLLSCAN: plan_stability_subjoin_cardinality_md.region {"r_regionkey":{"$not":{"$eq":1}}}
```
Estimated cardinality: 4  
Actual cardinality: 4  
Orders of magnitude: 0

---
### >>> Subjoin 57-1
```
db.region.aggregate(EJSON.deserialize(
[
{"$match":{"r_regionkey":{"$not":{"$eq":1}}}},
{"$lookup":{"from":"nation","localField":"r_regionkey","foreignField":"n_regionkey","as":"nation","pipeline":[
{"$match":{"n_name":{"$not":{"$eq":"MOROCCO"}}}}]}},
{"$unwind":"$nation"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$nation"]}}}]
));
```
Subjoin plan:
```
HJ r_regionkey = n_regionkey
  -> [region_s] COLLSCAN: plan_stability_subjoin_cardinality_md.region {"r_regionkey":{"$not":{"$eq":1}}} 
  -> [nation_s] COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"n_name":{"$not":{"$eq":"MOROCCO"}}}
```
Estimated cardinality: 19  
Actual cardinality: 19  
Orders of magnitude: 0

---
### >>> Subjoin 57-2
```
db.region.aggregate(EJSON.deserialize(
[
{"$match":{"r_regionkey":{"$not":{"$eq":1}}}},
{"$lookup":{"from":"nation","localField":"r_regionkey","foreignField":"n_regionkey","as":"nation","pipeline":[
{"$match":{"n_name":{"$not":{"$eq":"MOROCCO"}}}}]}},
{"$unwind":"$nation"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$nation"]}}},
{"$lookup":{"from":"supplier","localField":"n_nationkey","foreignField":"s_nationkey","as":"supplier","pipeline":[
{"$match":{"$or":[{"s_nationkey":{"$eq":15}},{"s_acctbal":{"$gt":9747.16}},{"s_name":{"$in":["Supplier#000000439","Supplier#000000517"]}}]}}]}},
{"$unwind":"$supplier"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$supplier"]}}}]
));
```
Subjoin plan:
```
HJ nation_s.n_nationkey = s_nationkey
  -> [none] HJ r_regionkey = n_regionkey
      -> [region_s] COLLSCAN: plan_stability_subjoin_cardinality_md.region {"r_regionkey":{"$not":{"$eq":1}}} 
      -> [nation_s] COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"n_name":{"$not":{"$eq":"MOROCCO"}}} 
  -> [supplier] COLLSCAN: plan_stability_subjoin_cardinality_md.supplier {"$or":[{"s_nationkey":{"$eq":15}},{"s_acctbal":{"$gt":9747.16}},{"s_name":{"$in":["Supplier#000000439","Supplier#000000517"]}}]}
```
Estimated cardinality: 49  
Actual cardinality: 19  
Orders of magnitude: 0

---
### >>> Subjoin 57-3
```
db.region.aggregate(EJSON.deserialize(
[
{"$match":{"r_regionkey":{"$not":{"$eq":1}}}},
{"$lookup":{"from":"nation","localField":"r_regionkey","foreignField":"n_regionkey","as":"nation","pipeline":[
{"$match":{"n_name":{"$not":{"$eq":"MOROCCO"}}}}]}},
{"$unwind":"$nation"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$nation"]}}},
{"$lookup":{"from":"supplier","localField":"n_nationkey","foreignField":"s_nationkey","as":"supplier","pipeline":[
{"$match":{"$or":[{"s_nationkey":{"$eq":15}},{"s_acctbal":{"$gt":9747.16}},{"s_name":{"$in":["Supplier#000000439","Supplier#000000517"]}}]}}]}},
{"$unwind":"$supplier"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$supplier"]}}},
{"$lookup":{"from":"partsupp","localField":"s_suppkey","foreignField":"ps_suppkey","as":"partsupp","pipeline":[
{"$match":{"$and":[{"ps_comment":{"$regex":"^in"}},{}]}}]}},
{"$unwind":"$partsupp"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$partsupp"]}}}]
));
```
Subjoin plan:
```
INLJ supplier.s_suppkey = ps_suppkey
  -> [none] HJ nation_s.n_nationkey = s_nationkey
      -> [none] HJ r_regionkey = n_regionkey
          -> [region_s] COLLSCAN: plan_stability_subjoin_cardinality_md.region {"r_regionkey":{"$not":{"$eq":1}}} 
          -> [nation_s] COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"n_name":{"$not":{"$eq":"MOROCCO"}}} 
      -> [supplier] COLLSCAN: plan_stability_subjoin_cardinality_md.supplier {"$or":[{"s_nationkey":{"$eq":15}},{"s_acctbal":{"$gt":9747.16}},{"s_name":{"$in":["Supplier#000000439","Supplier#000000517"]}}]} 
  -> [none] FETCH: plan_stability_subjoin_cardinality_md.partsupp {"ps_comment":{"$regex":"^in"}} 
      -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.partsupp ps_suppkey_1
```
Estimated cardinality: 102  
Actual cardinality: 24  
Orders of magnitude: 1

---
## >>> Command idx 58
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$and":[{"ps_availqty":{"$gte":5937}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"supplier","localField":"partsupp.ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$or":[{"s_name":"Supplier#000000690"},{"s_name":"Supplier#000000742"},{"s_name":"Supplier#000000148"}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$match":{"$nor":[{"p_container":{"$nin":["LG BAG","MED DRUM"]}},{"p_comment":{"$regex":{"$regex":"^ ","$options":""}}}]}}],"cursor":{},"idx":58}
```
### >>> Subjoin 58-0
```
db.supplier.aggregate(EJSON.deserialize(
[
{"$match":{"s_name":{"$in":["Supplier#000000148","Supplier#000000690","Supplier#000000742"]}}}]
));
```
Subjoin plan:
```
COLLSCAN: plan_stability_subjoin_cardinality_md.supplier {"s_name":{"$in":["Supplier#000000148","Supplier#000000690","Supplier#000000742"]}}
```
Estimated cardinality: 3  
Actual cardinality: 3  
Orders of magnitude: 0

---
### >>> Subjoin 58-1
```
db.supplier.aggregate(EJSON.deserialize(
[
{"$match":{"s_name":{"$in":["Supplier#000000148","Supplier#000000690","Supplier#000000742"]}}},
{"$lookup":{"from":"partsupp","localField":"s_suppkey","foreignField":"ps_suppkey","as":"partsupp","pipeline":[
{"$match":{"$and":[{"ps_availqty":{"$gte":5937}},{}]}}]}},
{"$unwind":"$partsupp"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$partsupp"]}}}]
));
```
Subjoin plan:
```
INLJ s_suppkey = ps_suppkey
  -> [supplier] COLLSCAN: plan_stability_subjoin_cardinality_md.supplier {"s_name":{"$in":["Supplier#000000148","Supplier#000000690","Supplier#000000742"]}} 
  -> [partsupp] FETCH: plan_stability_subjoin_cardinality_md.partsupp {"ps_availqty":{"$gte":5937}} 
      -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.partsupp ps_suppkey_1
```
Estimated cardinality: 96  
Actual cardinality: 86  
Orders of magnitude: 0

---
### >>> Subjoin 58-2
```
db.supplier.aggregate(EJSON.deserialize(
[
{"$match":{"s_name":{"$in":["Supplier#000000148","Supplier#000000690","Supplier#000000742"]}}},
{"$lookup":{"from":"partsupp","localField":"s_suppkey","foreignField":"ps_suppkey","as":"partsupp","pipeline":[
{"$match":{"$and":[{"ps_availqty":{"$gte":5937}},{}]}}]}},
{"$unwind":"$partsupp"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$partsupp"]}}},
{"$lookup":{"from":"part","localField":"ps_partkey","foreignField":"p_partkey","as":"part","pipeline":[
{"$match":{"$nor":[{"p_comment":{"$regex":"^ "}},{"p_container":{"$not":{"$in":["LG BAG","MED DRUM"]}}}]}}]}},
{"$unwind":"$part"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$part"]}}}]
));
```
Subjoin plan:
```
HJ partsupp.ps_partkey = p_partkey
  -> [none] INLJ s_suppkey = ps_suppkey
      -> [supplier] COLLSCAN: plan_stability_subjoin_cardinality_md.supplier {"s_name":{"$in":["Supplier#000000148","Supplier#000000690","Supplier#000000742"]}} 
      -> [partsupp] FETCH: plan_stability_subjoin_cardinality_md.partsupp {"ps_availqty":{"$gte":5937}} 
          -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.partsupp ps_suppkey_1
  -> [none] COLLSCAN: plan_stability_subjoin_cardinality_md.part {"$nor":[{"p_comment":{"$regex":"^ "}},{"p_container":{"$not":{"$in":["LG BAG","MED DRUM"]}}}]}
```
Estimated cardinality: 3  
Actual cardinality: 4  
Orders of magnitude: 0

---
## >>> Command idx 59
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$or":[{"ps_availqty":{"$eq":2826}},{"ps_supplycost":{"$gt":240.39}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"supplier","localField":"partsupp.ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$or":[{"s_nationkey":21},{"s_name":"Supplier#000000676"}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$nor":[{"n_regionkey":4}]}},
{"$match":{"$nor":[{"n_regionkey":3}]}},
{"$match":{"$nor":[{"n_name":{"$in":["VIETNAM","MOZAMBIQUE","CANADA"]}},{"n_regionkey":1}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$match":{"$nor":[{"p_type":"SMALL BRUSHED NICKEL"}]}}],"cursor":{},"idx":59}
```
### >>> Subjoin 59-0
```
db.nation.aggregate(EJSON.deserialize(
[
{"$match":{"$and":[{"n_regionkey":{"$not":{"$eq":4}}},{"n_regionkey":{"$not":{"$eq":3}}},{"$nor":[{"n_regionkey":{"$eq":1}},{"n_name":{"$in":["CANADA","MOZAMBIQUE","VIETNAM"]}}]}]}}]
));
```
Subjoin plan:
```
COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"$and":[{"n_regionkey":{"$not":{"$eq":4}}},{"n_regionkey":{"$not":{"$eq":3}}},{"$nor":[{"n_regionkey":{"$eq":1}},{"n_name":{"$in":["CANADA","MOZAMBIQUE","VIETNAM"]}}]}]}
```
Estimated cardinality: 8  
Actual cardinality: 8  
Orders of magnitude: 0

---
### >>> Subjoin 59-1
```
db.nation.aggregate(EJSON.deserialize(
[
{"$match":{"$and":[{"n_regionkey":{"$not":{"$eq":4}}},{"n_regionkey":{"$not":{"$eq":3}}},{"$nor":[{"n_regionkey":{"$eq":1}},{"n_name":{"$in":["CANADA","MOZAMBIQUE","VIETNAM"]}}]}]}},
{"$lookup":{"from":"supplier","localField":"n_nationkey","foreignField":"s_nationkey","as":"supplier","pipeline":[
{"$match":{"$or":[{"s_name":{"$eq":"Supplier#000000676"}},{"s_nationkey":{"$eq":21}}]}}]}},
{"$unwind":"$supplier"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$supplier"]}}}]
));
```
Subjoin plan:
```
HJ n_nationkey = s_nationkey
  -> [nation_s] COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"$and":[{"n_regionkey":{"$not":{"$eq":4}}},{"n_regionkey":{"$not":{"$eq":3}}},{"$nor":[{"n_regionkey":{"$eq":1}},{"n_name":{"$in":["CANADA","MOZAMBIQUE","VIETNAM"]}}]}]} 
  -> [supplier] COLLSCAN: plan_stability_subjoin_cardinality_md.supplier {"$or":[{"s_name":{"$eq":"Supplier#000000676"}},{"s_nationkey":{"$eq":21}}]}
```
Estimated cardinality: 13  
Actual cardinality: 1  
Orders of magnitude: 1

---
### >>> Subjoin 59-2
```
db.nation.aggregate(EJSON.deserialize(
[
{"$match":{"$and":[{"n_regionkey":{"$not":{"$eq":4}}},{"n_regionkey":{"$not":{"$eq":3}}},{"$nor":[{"n_regionkey":{"$eq":1}},{"n_name":{"$in":["CANADA","MOZAMBIQUE","VIETNAM"]}}]}]}},
{"$lookup":{"from":"supplier","localField":"n_nationkey","foreignField":"s_nationkey","as":"supplier","pipeline":[
{"$match":{"$or":[{"s_name":{"$eq":"Supplier#000000676"}},{"s_nationkey":{"$eq":21}}]}}]}},
{"$unwind":"$supplier"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$supplier"]}}},
{"$lookup":{"from":"partsupp","localField":"s_suppkey","foreignField":"ps_suppkey","as":"partsupp","pipeline":[
{"$match":{"$and":[{"$or":[{"ps_availqty":{"$eq":2826}},{"ps_supplycost":{"$gt":240.39}}]},{}]}}]}},
{"$unwind":"$partsupp"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$partsupp"]}}}]
));
```
Subjoin plan:
```
INLJ supplier.s_suppkey = ps_suppkey
  -> [none] HJ n_nationkey = s_nationkey
      -> [nation_s] COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"$and":[{"n_regionkey":{"$not":{"$eq":4}}},{"n_regionkey":{"$not":{"$eq":3}}},{"$nor":[{"n_regionkey":{"$eq":1}},{"n_name":{"$in":["CANADA","MOZAMBIQUE","VIETNAM"]}}]}]} 
      -> [supplier] COLLSCAN: plan_stability_subjoin_cardinality_md.supplier {"$or":[{"s_name":{"$eq":"Supplier#000000676"}},{"s_nationkey":{"$eq":21}}]} 
  -> [partsupp] FETCH: plan_stability_subjoin_cardinality_md.partsupp {"$or":[{"ps_availqty":{"$eq":2826}},{"ps_supplycost":{"$gt":240.39}}]} 
      -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.partsupp ps_suppkey_1
```
Estimated cardinality: 795  
Actual cardinality: 59  
Orders of magnitude: 1

---
### >>> Subjoin 59-3
```
db.nation.aggregate(EJSON.deserialize(
[
{"$match":{"$and":[{"n_regionkey":{"$not":{"$eq":4}}},{"n_regionkey":{"$not":{"$eq":3}}},{"$nor":[{"n_regionkey":{"$eq":1}},{"n_name":{"$in":["CANADA","MOZAMBIQUE","VIETNAM"]}}]}]}},
{"$lookup":{"from":"supplier","localField":"n_nationkey","foreignField":"s_nationkey","as":"supplier","pipeline":[
{"$match":{"$or":[{"s_name":{"$eq":"Supplier#000000676"}},{"s_nationkey":{"$eq":21}}]}}]}},
{"$unwind":"$supplier"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$supplier"]}}},
{"$lookup":{"from":"partsupp","localField":"s_suppkey","foreignField":"ps_suppkey","as":"partsupp","pipeline":[
{"$match":{"$and":[{"$or":[{"ps_availqty":{"$eq":2826}},{"ps_supplycost":{"$gt":240.39}}]},{}]}}]}},
{"$unwind":"$partsupp"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$partsupp"]}}},
{"$lookup":{"from":"part","localField":"ps_partkey","foreignField":"p_partkey","as":"part","pipeline":[
{"$match":{"p_type":{"$not":{"$eq":"SMALL BRUSHED NICKEL"}}}}]}},
{"$unwind":"$part"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$part"]}}}]
));
```
Subjoin plan:
```
HJ partsupp.ps_partkey = p_partkey
  -> [none] INLJ supplier.s_suppkey = ps_suppkey
      -> [none] HJ n_nationkey = s_nationkey
          -> [nation_s] COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"$and":[{"n_regionkey":{"$not":{"$eq":4}}},{"n_regionkey":{"$not":{"$eq":3}}},{"$nor":[{"n_regionkey":{"$eq":1}},{"n_name":{"$in":["CANADA","MOZAMBIQUE","VIETNAM"]}}]}]} 
          -> [supplier] COLLSCAN: plan_stability_subjoin_cardinality_md.supplier {"$or":[{"s_name":{"$eq":"Supplier#000000676"}},{"s_nationkey":{"$eq":21}}]} 
      -> [partsupp] FETCH: plan_stability_subjoin_cardinality_md.partsupp {"$or":[{"ps_availqty":{"$eq":2826}},{"ps_supplycost":{"$gt":240.39}}]} 
          -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.partsupp ps_suppkey_1
  -> [none] COLLSCAN: plan_stability_subjoin_cardinality_md.part {"p_type":{"$not":{"$eq":"SMALL BRUSHED NICKEL"}}}
```
Estimated cardinality: 789  
Actual cardinality: 59  
Orders of magnitude: 1

---
## >>> Command idx 60
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$and":[{"ps_comment":{"$regex":{"$regex":"^ ","$options":""}}}]}},
{"$match":{"$or":[{"ps_availqty":{"$gte":9820}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"supplier","localField":"partsupp.ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$and":[{"s_acctbal":{"$lt":6537.07}}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$or":[{"n_regionkey":1}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$match":{"$nor":[{"supplier.o_orderpriority":"5-LOW"}]}}],"cursor":{},"idx":60}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 61
```
{"aggregate":"partsupp","pipeline":[
{"$lookup":{"from":"supplier","localField":"ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$nor":[{"s_acctbal":{"$eq":5046.81}}]}},
{"$match":{"$and":[{"s_acctbal":{"$gte":3580.35}}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$and":[{"n_regionkey":4}]}},
{"$match":{"$and":[{"n_regionkey":4}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$lookup":{"from":"region","localField":"nation_s.n_regionkey","foreignField":"r_regionkey","pipeline":[
{"$match":{"$nor":[{"r_name":"EUROPE"}]}}],"as":"region_s"}},
{"$unwind":"$region_s"},
{"$match":{"$or":[{"supplier.o_orderdate":{"$lte":"1994-02-28T00:00:00.000Z"}},{"ps_comment":{"$regex":{"$regex":"^bl","$options":""}}}]}}],"cursor":{},"idx":61}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 62
```
{"aggregate":"supplier","pipeline":[
{"$lookup":{"from":"nation","localField":"s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$nor":[{"n_regionkey":0},{"n_regionkey":1}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$lookup":{"from":"region","localField":"nation_s.n_regionkey","foreignField":"r_regionkey","pipeline":[
{"$match":{"$nor":[{"r_regionkey":0},{"r_name":"AMERICA"}]}},
{"$match":{"$nor":[{"r_name":"EUROPE"}]}}],"as":"region_s"}},
{"$unwind":"$region_s"},
{"$match":{"$nor":[{"s_acctbal":{"$lt":-256.13}}]}}],"cursor":{},"idx":62}
```
### >>> Subjoin 62-0
```
db.region.aggregate(EJSON.deserialize(
[
{"$match":{"$and":[{"r_name":{"$not":{"$eq":"EUROPE"}}},{"$nor":[{"r_name":{"$eq":"AMERICA"}},{"r_regionkey":{"$eq":0}}]}]}}]
));
```
Subjoin plan:
```
COLLSCAN: plan_stability_subjoin_cardinality_md.region {"$and":[{"r_name":{"$not":{"$eq":"EUROPE"}}},{"$nor":[{"r_name":{"$eq":"AMERICA"}},{"r_regionkey":{"$eq":0}}]}]}
```
Estimated cardinality: 2  
Actual cardinality: 2  
Orders of magnitude: 0

---
### >>> Subjoin 62-1
```
db.region.aggregate(EJSON.deserialize(
[
{"$match":{"$and":[{"r_name":{"$not":{"$eq":"EUROPE"}}},{"$nor":[{"r_name":{"$eq":"AMERICA"}},{"r_regionkey":{"$eq":0}}]}]}},
{"$lookup":{"from":"nation","localField":"r_regionkey","foreignField":"n_regionkey","as":"nation","pipeline":[
{"$match":{"$nor":[{"n_regionkey":{"$eq":0}},{"n_regionkey":{"$eq":1}}]}}]}},
{"$unwind":"$nation"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$nation"]}}}]
));
```
Subjoin plan:
```
HJ r_regionkey = n_regionkey
  -> [region_s] COLLSCAN: plan_stability_subjoin_cardinality_md.region {"$and":[{"r_name":{"$not":{"$eq":"EUROPE"}}},{"$nor":[{"r_name":{"$eq":"AMERICA"}},{"r_regionkey":{"$eq":0}}]}]} 
  -> [nation_s] COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"$nor":[{"n_regionkey":{"$eq":0}},{"n_regionkey":{"$eq":1}}]}
```
Estimated cardinality: 6  
Actual cardinality: 10  
Orders of magnitude: 1

---
### >>> Subjoin 62-2
```
db.region.aggregate(EJSON.deserialize(
[
{"$match":{"$and":[{"r_name":{"$not":{"$eq":"EUROPE"}}},{"$nor":[{"r_name":{"$eq":"AMERICA"}},{"r_regionkey":{"$eq":0}}]}]}},
{"$lookup":{"from":"nation","localField":"r_regionkey","foreignField":"n_regionkey","as":"nation","pipeline":[
{"$match":{"$nor":[{"n_regionkey":{"$eq":0}},{"n_regionkey":{"$eq":1}}]}}]}},
{"$unwind":"$nation"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$nation"]}}},
{"$lookup":{"from":"supplier","localField":"n_nationkey","foreignField":"s_nationkey","as":"supplier","pipeline":[
{"$match":{"s_acctbal":{"$not":{"$lt":-256.13}}}}]}},
{"$unwind":"$supplier"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$supplier"]}}}]
));
```
Subjoin plan:
```
HJ nation_s.n_nationkey = s_nationkey
  -> [none] HJ r_regionkey = n_regionkey
      -> [region_s] COLLSCAN: plan_stability_subjoin_cardinality_md.region {"$and":[{"r_name":{"$not":{"$eq":"EUROPE"}}},{"$nor":[{"r_name":{"$eq":"AMERICA"}},{"r_regionkey":{"$eq":0}}]}]} 
      -> [nation_s] COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"$nor":[{"n_regionkey":{"$eq":0}},{"n_regionkey":{"$eq":1}}]} 
  -> [none] COLLSCAN: plan_stability_subjoin_cardinality_md.supplier {"s_acctbal":{"$not":{"$lt":-256.13}}}
```
Estimated cardinality: 224  
Actual cardinality: 396  
Orders of magnitude: 0

---
## >>> Command idx 63
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$or":[{"ps_availqty":{"$gte":7969}},{"ps_availqty":{"$gt":789}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"supplier","localField":"partsupp.ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$or":[{"s_name":"Supplier#000000949"}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$match":{"$or":[{"supplier.s_acctbal":{"$lt":6113.96}},{"p_mfgr":"Manufacturer#2"},{"partsupp.ps_availqty":{"$gt":3930}}]}}],"cursor":{},"idx":63}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 64
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$nor":[{"ps_availqty":{"$gt":8620}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"supplier","localField":"partsupp.ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$nor":[{"s_nationkey":8},{"s_nationkey":16}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$or":[{"n_name":{"$in":["PERU","ALGERIA","UNITED STATES","INDIA"]}},{"n_regionkey":1}]}},
{"$match":{"$nor":[{"n_name":"ALGERIA"}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$match":{"$or":[{"nation_s.n_name":{"$in":["ALGERIA","CHINA"]}},{"partsupp.ps_comment":{"$regex":{"$regex":"^quic","$options":""}}}]}}],"cursor":{},"idx":64}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 65
```
{"aggregate":"partsupp","pipeline":[
{"$lookup":{"from":"supplier","localField":"ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$and":[{"s_acctbal":{"$gte":2697.53}}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$or":[{"n_name":"JORDAN"}]}},
{"$match":{"$or":[{"n_regionkey":4}]}},
{"$match":{"$or":[{"n_regionkey":2},{"n_name":{"$nin":["VIETNAM","JAPAN","FRANCE"]}},{"n_name":"RUSSIA"},{"n_regionkey":0},{"n_regionkey":1}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$lookup":{"from":"region","localField":"nation_s.n_regionkey","foreignField":"r_regionkey","pipeline":[
{"$match":{"$nor":[{"r_regionkey":3}]}},
{"$match":{"$nor":[{"r_name":"AMERICA"},{"r_name":"EUROPE"}]}}],"as":"region_s"}},
{"$unwind":"$region_s"},
{"$match":{"$nor":[{"ps_availqty":{"$lte":4022}}]}}],"cursor":{},"idx":65}
```
### >>> Subjoin 65-0
```
db.nation.aggregate(EJSON.deserialize(
[
{"$match":{"$and":[{"$and":[{"$or":[{"n_name":{"$not":{"$in":["FRANCE","JAPAN","VIETNAM"]}}},{"n_name":{"$eq":"RUSSIA"}},{"n_regionkey":{"$in":[0,1,2]}}]},{"n_name":{"$eq":"JORDAN"}}]},{"n_regionkey":4}]}}]
));
```
Subjoin plan:
```
FETCH: plan_stability_subjoin_cardinality_md.nation {"$and":[{"$or":[{"n_name":{"$not":{"$in":["FRANCE","JAPAN","VIETNAM"]}}},{"n_name":{"$eq":"RUSSIA"}},{"n_regionkey":{"$in":[0,1,2]}}]},{"n_name":{"$eq":"JORDAN"}}]} 
  -> IXSCAN: plan_stability_subjoin_cardinality_md.nation n_regionkey_1 {"n_regionkey":["[4.0, 4.0]"]}
```
Estimated cardinality: 1  
Actual cardinality: 1  
Orders of magnitude: 0

---
### >>> Subjoin 65-1
```
db.nation.aggregate(EJSON.deserialize(
[
{"$match":{"$and":[{"$and":[{"$or":[{"n_name":{"$not":{"$in":["FRANCE","JAPAN","VIETNAM"]}}},{"n_name":{"$eq":"RUSSIA"}},{"n_regionkey":{"$in":[0,1,2]}}]},{"n_name":{"$eq":"JORDAN"}}]},{"n_regionkey":4}]}},
{"$lookup":{"from":"region","localField":"n_regionkey","foreignField":"r_regionkey","as":"region","pipeline":[
{"$match":{"$and":[{"r_regionkey":{"$not":{"$eq":3}}},{"$nor":[{"r_name":{"$eq":"AMERICA"}},{"r_name":{"$eq":"EUROPE"}}]}]}}]}},
{"$unwind":"$region"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$region"]}}}]
));
```
Subjoin plan:
```
NLJ n_regionkey = r_regionkey
  -> [nation_s] FETCH: plan_stability_subjoin_cardinality_md.nation {"$and":[{"$or":[{"n_name":{"$not":{"$in":["FRANCE","JAPAN","VIETNAM"]}}},{"n_name":{"$eq":"RUSSIA"}},{"n_regionkey":{"$in":[0,1,2]}}]},{"n_name":{"$eq":"JORDAN"}}]} 
      -> IXSCAN: plan_stability_subjoin_cardinality_md.nation n_regionkey_1 {"n_regionkey":["[4.0, 4.0]"]}
  -> [region_s] COLLSCAN: plan_stability_subjoin_cardinality_md.region {"$and":[{"r_regionkey":{"$not":{"$eq":3}}},{"$nor":[{"r_name":{"$eq":"AMERICA"}},{"r_name":{"$eq":"EUROPE"}}]}]}
```
Estimated cardinality: 1  
Actual cardinality: 1  
Orders of magnitude: 0

---
### >>> Subjoin 65-2
```
db.nation.aggregate(EJSON.deserialize(
[
{"$match":{"$and":[{"$and":[{"$or":[{"n_name":{"$not":{"$in":["FRANCE","JAPAN","VIETNAM"]}}},{"n_name":{"$eq":"RUSSIA"}},{"n_regionkey":{"$in":[0,1,2]}}]},{"n_name":{"$eq":"JORDAN"}}]},{"n_regionkey":4}]}},
{"$lookup":{"from":"region","localField":"n_regionkey","foreignField":"r_regionkey","as":"region","pipeline":[
{"$match":{"$and":[{"r_regionkey":{"$not":{"$eq":3}}},{"$nor":[{"r_name":{"$eq":"AMERICA"}},{"r_name":{"$eq":"EUROPE"}}]}]}}]}},
{"$unwind":"$region"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$region"]}}},
{"$lookup":{"from":"supplier","localField":"n_nationkey","foreignField":"s_nationkey","as":"supplier","pipeline":[
{"$match":{"$and":[{"s_acctbal":{"$gte":2697.53}},{}]}}]}},
{"$unwind":"$supplier"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$supplier"]}}}]
));
```
Subjoin plan:
```
INLJ nation_s.n_nationkey = s_nationkey
  -> [none] NLJ n_regionkey = r_regionkey
      -> [nation_s] FETCH: plan_stability_subjoin_cardinality_md.nation {"$and":[{"$or":[{"n_name":{"$not":{"$in":["FRANCE","JAPAN","VIETNAM"]}}},{"n_name":{"$eq":"RUSSIA"}},{"n_regionkey":{"$in":[0,1,2]}}]},{"n_name":{"$eq":"JORDAN"}}]} 
          -> IXSCAN: plan_stability_subjoin_cardinality_md.nation n_regionkey_1 {"n_regionkey":["[4.0, 4.0]"]}
      -> [region_s] COLLSCAN: plan_stability_subjoin_cardinality_md.region {"$and":[{"r_regionkey":{"$not":{"$eq":3}}},{"$nor":[{"r_name":{"$eq":"AMERICA"}},{"r_name":{"$eq":"EUROPE"}}]}]} 
  -> [supplier] FETCH: plan_stability_subjoin_cardinality_md.supplier {"s_acctbal":{"$gte":2697.53}} 
      -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.supplier s_nationkey_1
```
Estimated cardinality: 16  
Actual cardinality: 17  
Orders of magnitude: 0

---
### >>> Subjoin 65-3
```
db.nation.aggregate(EJSON.deserialize(
[
{"$match":{"$and":[{"$and":[{"$or":[{"n_name":{"$not":{"$in":["FRANCE","JAPAN","VIETNAM"]}}},{"n_name":{"$eq":"RUSSIA"}},{"n_regionkey":{"$in":[0,1,2]}}]},{"n_name":{"$eq":"JORDAN"}}]},{"n_regionkey":4}]}},
{"$lookup":{"from":"region","localField":"n_regionkey","foreignField":"r_regionkey","as":"region","pipeline":[
{"$match":{"$and":[{"r_regionkey":{"$not":{"$eq":3}}},{"$nor":[{"r_name":{"$eq":"AMERICA"}},{"r_name":{"$eq":"EUROPE"}}]}]}}]}},
{"$unwind":"$region"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$region"]}}},
{"$lookup":{"from":"supplier","localField":"n_nationkey","foreignField":"s_nationkey","as":"supplier","pipeline":[
{"$match":{"$and":[{"s_acctbal":{"$gte":2697.53}},{}]}}]}},
{"$unwind":"$supplier"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$supplier"]}}},
{"$lookup":{"from":"partsupp","localField":"s_suppkey","foreignField":"ps_suppkey","as":"partsupp","pipeline":[
{"$match":{"$and":[{"ps_availqty":{"$not":{"$lte":4022}}},{}]}}]}},
{"$unwind":"$partsupp"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$partsupp"]}}}]
));
```
Subjoin plan:
```
INLJ supplier.s_suppkey = ps_suppkey
  -> [none] INLJ nation_s.n_nationkey = s_nationkey
      -> [none] NLJ n_regionkey = r_regionkey
          -> [nation_s] FETCH: plan_stability_subjoin_cardinality_md.nation {"$and":[{"$or":[{"n_name":{"$not":{"$in":["FRANCE","JAPAN","VIETNAM"]}}},{"n_name":{"$eq":"RUSSIA"}},{"n_regionkey":{"$in":[0,1,2]}}]},{"n_name":{"$eq":"JORDAN"}}]} 
              -> IXSCAN: plan_stability_subjoin_cardinality_md.nation n_regionkey_1 {"n_regionkey":["[4.0, 4.0]"]}
          -> [region_s] COLLSCAN: plan_stability_subjoin_cardinality_md.region {"$and":[{"r_regionkey":{"$not":{"$eq":3}}},{"$nor":[{"r_name":{"$eq":"AMERICA"}},{"r_name":{"$eq":"EUROPE"}}]}]} 
      -> [supplier] FETCH: plan_stability_subjoin_cardinality_md.supplier {"s_acctbal":{"$gte":2697.53}} 
          -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.supplier s_nationkey_1
  -> [none] FETCH: plan_stability_subjoin_cardinality_md.partsupp {"ps_availqty":{"$not":{"$lte":4022}}} 
      -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.partsupp ps_suppkey_1
```
Estimated cardinality: 745  
Actual cardinality: 828  
Orders of magnitude: 0

---
## >>> Command idx 66
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$and":[{"ps_supplycost":{"$lte":146.47}},{"ps_availqty":{"$gt":5069}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"supplier","localField":"partsupp.ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$nor":[{"s_acctbal":{"$gte":8724.42}}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$or":[{"n_regionkey":1}]}},
{"$match":{"$nor":[{"n_name":{"$nin":["INDIA","UNITED STATES","CANADA"]}},{"n_regionkey":0}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$match":{"$nor":[{"supplier.o_orderpriority":"5-LOW"}]}}],"cursor":{},"idx":66}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 67
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$or":[{"ps_comment":{"$regex":{"$regex":"^f","$options":""}}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"supplier","localField":"partsupp.ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$nor":[{"s_nationkey":2},{"s_acctbal":{"$eq":10.33}},{"s_name":"Supplier#000000824"}]}},
{"$match":{"$or":[{"s_nationkey":2},{"s_acctbal":{"$lt":1033.1}}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$or":[{"n_regionkey":0},{"n_name":{"$nin":["CANADA","UNITED STATES"]}}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$match":{"$nor":[{"supplier.o_orderpriority":"1-URGENT"},{"supplier.s_name":"Supplier#000000906"}]}}],"cursor":{},"idx":67}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 68
```
{"aggregate":"supplier","pipeline":[
{"$lookup":{"from":"nation","localField":"s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$or":[{"n_regionkey":0},{"n_name":{"$in":["INDIA","IRAN"]}}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$lookup":{"from":"region","localField":"nation_s.n_regionkey","foreignField":"r_regionkey","pipeline":[
{"$match":{"$nor":[{"r_regionkey":2},{"r_name":"AMERICA"},{"r_regionkey":0}]}}],"as":"region_s"}},
{"$unwind":"$region_s"},
{"$match":{"$nor":[{"region_s.r_regionkey":3},{"s_name":"Supplier#000000250"}]}}],"cursor":{},"idx":68}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 69
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$nor":[{"ps_availqty":{"$gt":6256}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"supplier","localField":"partsupp.ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$and":[{"s_acctbal":{"$lt":7337.45}}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$match":{"$or":[{"supplier.s_name":"Supplier#000000527"},{"p_container":"SM CAN"}]}}],"cursor":{},"idx":69}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 70
```
{"aggregate":"supplier","pipeline":[
{"$lookup":{"from":"nation","localField":"s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$nor":[{"n_regionkey":0},{"n_name":"RUSSIA"}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$lookup":{"from":"region","localField":"nation_s.n_regionkey","foreignField":"r_regionkey","pipeline":[
{"$match":{"$nor":[{"r_name":"AMERICA"}]}}],"as":"region_s"}},
{"$unwind":"$region_s"},
{"$match":{"$nor":[{"s_name":"Supplier#000000686"},{"region_s.r_regionkey":3},{"s_acctbal":{"$gt":505.92}}]}}],"cursor":{},"idx":70}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 71
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$nor":[{"ps_supplycost":{"$eq":770.46}},{"ps_comment":{"$regex":{"$regex":"^quic","$options":""}}},{"ps_supplycost":{"$lt":546.77}}]}},
{"$match":{"$or":[{"ps_availqty":{"$lte":724}},{"ps_availqty":{"$gt":9149}},{"ps_comment":{"$regex":{"$regex":"^y r","$options":""}}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"supplier","localField":"partsupp.ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$nor":[{"s_nationkey":8}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$or":[{"n_name":"EGYPT"},{"n_name":"ETHIOPIA"},{"n_name":"MOZAMBIQUE"}]}},
{"$match":{"$nor":[{"n_regionkey":2}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$lookup":{"from":"region","localField":"nation_s.n_regionkey","foreignField":"r_regionkey","pipeline":[
{"$match":{"$or":[{"r_name":"AFRICA"},{"r_name":"MIDDLE EAST"}]}},
{"$match":{"$nor":[{"r_name":"AMERICA"}]}}],"as":"region_s"}},
{"$unwind":"$region_s"},
{"$match":{"$or":[{"nation_s.n_regionkey":0},{"nation_s.n_regionkey":0}]}}],"cursor":{},"idx":71}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 72
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$or":[{"ps_availqty":{"$lte":9607}},{"ps_availqty":{"$lt":2592}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"supplier","localField":"partsupp.ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$nor":[{"s_name":"Supplier#000000319"},{"s_acctbal":{"$gte":8210.13}},{"s_acctbal":{"$gte":6835.16}}]}},
{"$match":{"$or":[{"s_nationkey":13},{"s_name":"Supplier#000000517"},{"s_name":"Supplier#000000119"},{"s_acctbal":{"$lt":2461.11}}]}},
{"$match":{"$and":[{"s_acctbal":{"$gt":2060.13}},{"s_acctbal":{"$lte":8436.92}}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$or":[{"n_regionkey":1},{"n_regionkey":2},{"n_regionkey":3}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$lookup":{"from":"region","localField":"nation_s.n_regionkey","foreignField":"r_regionkey","pipeline":[
{"$match":{"$or":[{"r_name":"AMERICA"}]}},
{"$match":{"$nor":[{"r_name":"MIDDLE EAST"},{"r_name":"ASIA"}]}}],"as":"region_s"}},
{"$unwind":"$region_s"},
{"$match":{"$or":[{"region_s.r_name":"MIDDLE EAST"},{"partsupp.ps_supplycost":{"$lte":804.73}}]}}],"cursor":{},"idx":72}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 73
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$or":[{"ps_comment":{"$regex":{"$regex":"^n","$options":""}}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"supplier","localField":"partsupp.ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$nor":[{"s_nationkey":23},{"s_acctbal":{"$gt":8924.02}},{"s_nationkey":24}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$nor":[{"n_name":{"$in":["GERMANY","RUSSIA","PERU"]}},{"n_regionkey":3}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$lookup":{"from":"region","localField":"nation_s.n_regionkey","foreignField":"r_regionkey","pipeline":[
{"$match":{"$or":[{"r_name":"ASIA"},{"r_regionkey":0}]}}],"as":"region_s"}},
{"$unwind":"$region_s"},
{"$match":{"$or":[{"p_comment":{"$regex":{"$regex":"^e","$options":""}}},{"partsupp.ps_supplycost":{"$lte":324.34}}]}}],"cursor":{},"idx":73}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 74
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$nor":[{"ps_supplycost":{"$eq":240.39}},{"ps_comment":{"$regex":{"$regex":"^ly ","$options":""}}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"supplier","localField":"partsupp.ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$nor":[{"s_nationkey":8}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$nor":[{"n_name":"UNITED STATES"}]}},
{"$match":{"$and":[{"n_name":{"$in":["FRANCE","IRAQ"]}}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$match":{"$or":[{"nation_s.n_regionkey":1},{"partsupp.ps_comment":{"$regex":{"$regex":"^ e","$options":""}}},{"nation_s.n_regionkey":2}]}}],"cursor":{},"idx":74}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 75
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$or":[{"ps_comment":{"$regex":{"$regex":"^lu","$options":""}}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"supplier","localField":"partsupp.ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$nor":[{"s_nationkey":19}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$match":{"$nor":[{"supplier.s_name":"Supplier#000000439"}]}}],"cursor":{},"idx":75}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 76
```
{"aggregate":"partsupp","pipeline":[
{"$lookup":{"from":"supplier","localField":"ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$or":[{"s_acctbal":{"$lte":10.33}}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$nor":[{"n_name":{"$in":["FRANCE","IRAQ","ALGERIA"]}},{"n_regionkey":4}]}},
{"$match":{"$or":[{"n_name":"EGYPT"},{"n_name":{"$in":["UNITED KINGDOM","MOZAMBIQUE","ROMANIA"]}},{"n_regionkey":0}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$lookup":{"from":"region","localField":"nation_s.n_regionkey","foreignField":"r_regionkey","pipeline":[
{"$match":{"$and":[{"r_regionkey":3}]}}],"as":"region_s"}},
{"$unwind":"$region_s"},
{"$match":{"$or":[{"ps_availqty":{"$gte":6104}},{"nation_s.n_regionkey":1}]}}],"cursor":{},"idx":76}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 77
```
{"aggregate":"partsupp","pipeline":[
{"$lookup":{"from":"supplier","localField":"ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$or":[{"s_acctbal":{"$gte":7627.85}},{"s_acctbal":{"$lt":9681.99}},{"s_nationkey":19},{"s_name":"Supplier#000000719"}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$nor":[{"n_regionkey":2}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$lookup":{"from":"region","localField":"nation_s.n_regionkey","foreignField":"r_regionkey","pipeline":[
{"$match":{"$nor":[{"r_name":"AFRICA"},{"r_name":"EUROPE"},{"r_regionkey":3}]}},
{"$match":{"$nor":[{"r_name":"AMERICA"}]}},
{"$match":{"$nor":[{"r_regionkey":3}]}}],"as":"region_s"}},
{"$unwind":"$region_s"},
{"$match":{"$or":[{"ps_comment":{"$regex":{"$regex":"^req","$options":""}}}]}}],"cursor":{},"idx":77}
```
### >>> Subjoin 77-0
```
db.region.aggregate(EJSON.deserialize(
[
{"$match":{"$and":[{"r_name":{"$not":{"$eq":"AMERICA"}}},{"r_regionkey":{"$not":{"$eq":3}}},{"$nor":[{"r_name":{"$eq":"AFRICA"}},{"r_name":{"$eq":"EUROPE"}},{"r_regionkey":{"$eq":3}}]}]}}]
));
```
Subjoin plan:
```
COLLSCAN: plan_stability_subjoin_cardinality_md.region {"$and":[{"r_name":{"$not":{"$eq":"AMERICA"}}},{"r_regionkey":{"$not":{"$eq":3}}},{"$nor":[{"r_name":{"$eq":"AFRICA"}},{"r_name":{"$eq":"EUROPE"}},{"r_regionkey":{"$eq":3}}]}]}
```
Estimated cardinality: 2  
Actual cardinality: 2  
Orders of magnitude: 0

---
### >>> Subjoin 77-1
```
db.region.aggregate(EJSON.deserialize(
[
{"$match":{"$and":[{"r_name":{"$not":{"$eq":"AMERICA"}}},{"r_regionkey":{"$not":{"$eq":3}}},{"$nor":[{"r_name":{"$eq":"AFRICA"}},{"r_name":{"$eq":"EUROPE"}},{"r_regionkey":{"$eq":3}}]}]}},
{"$lookup":{"from":"nation","localField":"r_regionkey","foreignField":"n_regionkey","as":"nation","pipeline":[
{"$match":{"n_regionkey":{"$not":{"$eq":2}}}}]}},
{"$unwind":"$nation"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$nation"]}}}]
));
```
Subjoin plan:
```
HJ r_regionkey = n_regionkey
  -> [region_s] COLLSCAN: plan_stability_subjoin_cardinality_md.region {"$and":[{"r_name":{"$not":{"$eq":"AMERICA"}}},{"r_regionkey":{"$not":{"$eq":3}}},{"$nor":[{"r_name":{"$eq":"AFRICA"}},{"r_name":{"$eq":"EUROPE"}},{"r_regionkey":{"$eq":3}}]}]} 
  -> [nation_s] COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"n_regionkey":{"$not":{"$eq":2}}}
```
Estimated cardinality: 8  
Actual cardinality: 5  
Orders of magnitude: 0

---
### >>> Subjoin 77-2
```
db.region.aggregate(EJSON.deserialize(
[
{"$match":{"$and":[{"r_name":{"$not":{"$eq":"AMERICA"}}},{"r_regionkey":{"$not":{"$eq":3}}},{"$nor":[{"r_name":{"$eq":"AFRICA"}},{"r_name":{"$eq":"EUROPE"}},{"r_regionkey":{"$eq":3}}]}]}},
{"$lookup":{"from":"nation","localField":"r_regionkey","foreignField":"n_regionkey","as":"nation","pipeline":[
{"$match":{"n_regionkey":{"$not":{"$eq":2}}}}]}},
{"$unwind":"$nation"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$nation"]}}},
{"$lookup":{"from":"supplier","localField":"n_nationkey","foreignField":"s_nationkey","as":"supplier","pipeline":[
{"$match":{"$or":[{"s_name":{"$eq":"Supplier#000000719"}},{"s_nationkey":{"$eq":19}},{"s_acctbal":{"$lt":9681.99}},{"s_acctbal":{"$gte":7627.85}}]}}]}},
{"$unwind":"$supplier"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$supplier"]}}}]
));
```
Subjoin plan:
```
HJ nation_s.n_nationkey = s_nationkey
  -> [none] HJ r_regionkey = n_regionkey
      -> [region_s] COLLSCAN: plan_stability_subjoin_cardinality_md.region {"$and":[{"r_name":{"$not":{"$eq":"AMERICA"}}},{"r_regionkey":{"$not":{"$eq":3}}},{"$nor":[{"r_name":{"$eq":"AFRICA"}},{"r_name":{"$eq":"EUROPE"}},{"r_regionkey":{"$eq":3}}]}]} 
      -> [nation_s] COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"n_regionkey":{"$not":{"$eq":2}}} 
  -> [supplier] COLLSCAN: plan_stability_subjoin_cardinality_md.supplier {"$or":[{"s_name":{"$eq":"Supplier#000000719"}},{"s_nationkey":{"$eq":19}},{"s_acctbal":{"$lt":9681.99}},{"s_acctbal":{"$gte":7627.85}}]}
```
Estimated cardinality: 320  
Actual cardinality: 198  
Orders of magnitude: 0

---
### >>> Subjoin 77-3
```
db.region.aggregate(EJSON.deserialize(
[
{"$match":{"$and":[{"r_name":{"$not":{"$eq":"AMERICA"}}},{"r_regionkey":{"$not":{"$eq":3}}},{"$nor":[{"r_name":{"$eq":"AFRICA"}},{"r_name":{"$eq":"EUROPE"}},{"r_regionkey":{"$eq":3}}]}]}},
{"$lookup":{"from":"nation","localField":"r_regionkey","foreignField":"n_regionkey","as":"nation","pipeline":[
{"$match":{"n_regionkey":{"$not":{"$eq":2}}}}]}},
{"$unwind":"$nation"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$nation"]}}},
{"$lookup":{"from":"supplier","localField":"n_nationkey","foreignField":"s_nationkey","as":"supplier","pipeline":[
{"$match":{"$or":[{"s_name":{"$eq":"Supplier#000000719"}},{"s_nationkey":{"$eq":19}},{"s_acctbal":{"$lt":9681.99}},{"s_acctbal":{"$gte":7627.85}}]}}]}},
{"$unwind":"$supplier"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$supplier"]}}},
{"$lookup":{"from":"partsupp","localField":"s_suppkey","foreignField":"ps_suppkey","as":"partsupp","pipeline":[
{"$match":{"ps_comment":{"$regex":"^req"}}}]}},
{"$unwind":"$partsupp"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$partsupp"]}}}]
));
```
Subjoin plan:
```
HJ supplier.s_suppkey = ps_suppkey
  -> [none] HJ nation_s.n_nationkey = s_nationkey
      -> [none] HJ r_regionkey = n_regionkey
          -> [region_s] COLLSCAN: plan_stability_subjoin_cardinality_md.region {"$and":[{"r_name":{"$not":{"$eq":"AMERICA"}}},{"r_regionkey":{"$not":{"$eq":3}}},{"$nor":[{"r_name":{"$eq":"AFRICA"}},{"r_name":{"$eq":"EUROPE"}},{"r_regionkey":{"$eq":3}}]}]} 
          -> [nation_s] COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"n_regionkey":{"$not":{"$eq":2}}} 
      -> [supplier] COLLSCAN: plan_stability_subjoin_cardinality_md.supplier {"$or":[{"s_name":{"$eq":"Supplier#000000719"}},{"s_nationkey":{"$eq":19}},{"s_acctbal":{"$lt":9681.99}},{"s_acctbal":{"$gte":7627.85}}]} 
  -> [none] COLLSCAN: plan_stability_subjoin_cardinality_md.partsupp {"ps_comment":{"$regex":"^req"}}
```
Estimated cardinality: 128  
Actual cardinality: 68  
Orders of magnitude: 1

---
## >>> Command idx 78
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$or":[{"ps_availqty":{"$lt":854}}]}},
{"$match":{"$and":[{"ps_supplycost":{"$lte":161.52}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"supplier","localField":"partsupp.ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$nor":[{"s_nationkey":5},{"s_acctbal":{"$gt":9166.95}},{"s_acctbal":{"$gt":9537.73}}]}},
{"$match":{"$or":[{"s_nationkey":17},{"s_nationkey":2},{"s_acctbal":{"$lte":-685.94}}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$nor":[{"n_regionkey":4}]}},
{"$match":{"$or":[{"n_name":{"$nin":["MOZAMBIQUE","SAUDI ARABIA"]}},{"n_name":{"$in":["UNITED STATES","IRAN"]}}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$lookup":{"from":"region","localField":"nation_s.n_regionkey","foreignField":"r_regionkey","pipeline":[
{"$match":{"$or":[{"r_regionkey":1},{"r_regionkey":2}]}}],"as":"region_s"}},
{"$unwind":"$region_s"},
{"$match":{"$nor":[{"nation_s.n_regionkey":1}]}}],"cursor":{},"idx":78}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 79
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$and":[{"ps_supplycost":{"$gte":997.82}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"supplier","localField":"partsupp.ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$and":[{"s_acctbal":{"$lte":3222.71}}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$nor":[{"n_name":"KENYA"}]}},
{"$match":{"$nor":[{"n_name":"CANADA"},{"n_regionkey":3}]}},
{"$match":{"$nor":[{"n_name":{"$nin":["IRAQ","GERMANY","MOZAMBIQUE"]}},{"n_regionkey":4}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$lookup":{"from":"region","localField":"nation_s.n_regionkey","foreignField":"r_regionkey","pipeline":[
{"$match":{"$nor":[{"r_name":"MIDDLE EAST"},{"r_name":"ASIA"},{"r_regionkey":1}]}}],"as":"region_s"}},
{"$unwind":"$region_s"},
{"$match":{"$nor":[{"supplier.s_acctbal":{"$eq":868.36}}]}}],"cursor":{},"idx":79}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 80
```
{"aggregate":"lineitem","pipeline":[
{"$lookup":{"from":"orders","localField":"l_orderkey","foreignField":"o_orderkey","pipeline":[
{"$match":{"$nor":[{"o_clerk":"Clerk#000000567"}]}}],"as":"orders"}},
{"$unwind":"$orders"},
{"$lookup":{"from":"customer","localField":"orders.o_custkey","foreignField":"c_custkey","pipeline":[
{"$match":{"$nor":[{"c_mktsegment":"BUILDING"}]}}],"as":"customer"}},
{"$unwind":"$customer"},
{"$lookup":{"from":"supplier","localField":"customer.c_nationkey","foreignField":"s_nationkey","pipeline":[
{"$match":{"$and":[{"s_name":"Supplier#000000949"},{"s_acctbal":{"$lt":4663.08}}]}},
{"$match":{"$and":[{"s_nationkey":23}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$match":{"$or":[{"l_commitdate":{"$lte":"1992-04-13T00:00:00.000Z"}}]}}],"cursor":{},"idx":80}
```
### >>> Subjoin 80-0
```
db.supplier.aggregate(EJSON.deserialize(
[
{"$match":{"$and":[{"$and":[{"s_acctbal":{"$lt":4663.08}},{"s_name":{"$eq":"Supplier#000000949"}}]},{"s_nationkey":23}]}}]
));
```
Subjoin plan:
```
FETCH: plan_stability_subjoin_cardinality_md.supplier {"$and":[{"s_acctbal":{"$lt":4663.08}},{"s_name":{"$eq":"Supplier#000000949"}}]} 
  -> IXSCAN: plan_stability_subjoin_cardinality_md.supplier s_nationkey_1 {"s_nationkey":["[23.0, 23.0]"]}
```
Estimated cardinality: 1  
Actual cardinality: 1  
Orders of magnitude: 0

---
### >>> Subjoin 80-1
```
db.supplier.aggregate(EJSON.deserialize(
[
{"$match":{"$and":[{"$and":[{"s_acctbal":{"$lt":4663.08}},{"s_name":{"$eq":"Supplier#000000949"}}]},{"s_nationkey":23}]}},
{"$lookup":{"from":"customer","localField":"s_nationkey","foreignField":"c_nationkey","as":"customer","pipeline":[
{"$match":{"$and":[{"c_mktsegment":{"$not":{"$eq":"BUILDING"}}},{}]}}]}},
{"$unwind":"$customer"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$customer"]}}}]
));
```
Subjoin plan:
```
INLJ s_nationkey = c_nationkey
  -> [supplier] FETCH: plan_stability_subjoin_cardinality_md.supplier {"$and":[{"s_acctbal":{"$lt":4663.08}},{"s_name":{"$eq":"Supplier#000000949"}}]} 
      -> IXSCAN: plan_stability_subjoin_cardinality_md.supplier s_nationkey_1 {"s_nationkey":["[23.0, 23.0]"]}
  -> [customer] FETCH: plan_stability_subjoin_cardinality_md.customer {"c_mktsegment":{"$not":{"$eq":"BUILDING"}}} 
      -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.customer c_nationkey_1
```
Estimated cardinality: 468  
Actual cardinality: 486  
Orders of magnitude: 0

---
### >>> Subjoin 80-2
```
db.supplier.aggregate(EJSON.deserialize(
[
{"$match":{"$and":[{"$and":[{"s_acctbal":{"$lt":4663.08}},{"s_name":{"$eq":"Supplier#000000949"}}]},{"s_nationkey":23}]}},
{"$lookup":{"from":"customer","localField":"s_nationkey","foreignField":"c_nationkey","as":"customer","pipeline":[
{"$match":{"$and":[{"c_mktsegment":{"$not":{"$eq":"BUILDING"}}},{}]}}]}},
{"$unwind":"$customer"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$customer"]}}},
{"$lookup":{"from":"orders","localField":"c_custkey","foreignField":"o_custkey","as":"orders","pipeline":[
{"$match":{"$and":[{"o_clerk":{"$not":{"$eq":"Clerk#000000567"}}},{}]}}]}},
{"$unwind":"$orders"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$orders"]}}}]
));
```
Subjoin plan:
```
INLJ customer.c_custkey = o_custkey
  -> [none] INLJ s_nationkey = c_nationkey
      -> [supplier] FETCH: plan_stability_subjoin_cardinality_md.supplier {"$and":[{"s_acctbal":{"$lt":4663.08}},{"s_name":{"$eq":"Supplier#000000949"}}]} 
          -> IXSCAN: plan_stability_subjoin_cardinality_md.supplier s_nationkey_1 {"s_nationkey":["[23.0, 23.0]"]}
      -> [customer] FETCH: plan_stability_subjoin_cardinality_md.customer {"c_mktsegment":{"$not":{"$eq":"BUILDING"}}} 
          -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.customer c_nationkey_1
  -> [orders] FETCH: plan_stability_subjoin_cardinality_md.orders {"o_clerk":{"$not":{"$eq":"Clerk#000000567"}}} 
      -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.orders o_custkey_1
```
Estimated cardinality: 4675  
Actual cardinality: 4548  
Orders of magnitude: 0

---
### >>> Subjoin 80-3
```
db.supplier.aggregate(EJSON.deserialize(
[
{"$match":{"$and":[{"$and":[{"s_acctbal":{"$lt":4663.08}},{"s_name":{"$eq":"Supplier#000000949"}}]},{"s_nationkey":23}]}},
{"$lookup":{"from":"customer","localField":"s_nationkey","foreignField":"c_nationkey","as":"customer","pipeline":[
{"$match":{"$and":[{"c_mktsegment":{"$not":{"$eq":"BUILDING"}}},{}]}}]}},
{"$unwind":"$customer"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$customer"]}}},
{"$lookup":{"from":"orders","localField":"c_custkey","foreignField":"o_custkey","as":"orders","pipeline":[
{"$match":{"$and":[{"o_clerk":{"$not":{"$eq":"Clerk#000000567"}}},{}]}}]}},
{"$unwind":"$orders"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$orders"]}}},
{"$lookup":{"from":"lineitem","localField":"o_orderkey","foreignField":"l_orderkey","as":"lineitem","pipeline":[
{"$match":{"l_commitdate":{"$gte":null,"$lte":"1992-04-13T00:00:00.000Z"}}}]}},
{"$unwind":"$lineitem"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$lineitem"]}}}]
));
```
Subjoin plan:
```
HJ l_orderkey = orders.o_orderkey
  -> [none] FETCH: plan_stability_subjoin_cardinality_md.lineitem 
      -> IXSCAN: plan_stability_subjoin_cardinality_md.lineitem l_commitdate_1 {"l_commitdate":["[new Date(-9223372036854775808), new Date(703123200000)]"]}
  -> [none] INLJ customer.c_custkey = o_custkey
      -> [none] INLJ s_nationkey = c_nationkey
          -> [supplier] FETCH: plan_stability_subjoin_cardinality_md.supplier {"$and":[{"s_acctbal":{"$lt":4663.08}},{"s_name":{"$eq":"Supplier#000000949"}}]} 
              -> IXSCAN: plan_stability_subjoin_cardinality_md.supplier s_nationkey_1 {"s_nationkey":["[23.0, 23.0]"]}
          -> [customer] FETCH: plan_stability_subjoin_cardinality_md.customer {"c_mktsegment":{"$not":{"$eq":"BUILDING"}}} 
              -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.customer c_nationkey_1
      -> [orders] FETCH: plan_stability_subjoin_cardinality_md.orders {"o_clerk":{"$not":{"$eq":"Clerk#000000567"}}} 
          -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.orders o_custkey_1
```
Estimated cardinality: 94  
Actual cardinality: 387  
Orders of magnitude: 1

---
## >>> Command idx 81
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$or":[{"ps_availqty":{"$gte":756}},{"ps_supplycost":{"$lt":941.7}},{"ps_availqty":{"$gt":5124}}]}},
{"$match":{"$and":[{"ps_availqty":{"$lte":854}},{"ps_comment":{"$regex":{"$regex":"^ ","$options":""}}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"supplier","localField":"partsupp.ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$and":[{"s_acctbal":{"$gt":4269.56}}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$or":[{"n_name":{"$nin":["RUSSIA","FRANCE","RUSSIA"]}}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$lookup":{"from":"region","localField":"nation_s.n_regionkey","foreignField":"r_regionkey","pipeline":[
{"$match":{"$nor":[{"r_name":"EUROPE"}]}},
{"$match":{"$nor":[{"r_name":"MIDDLE EAST"}]}}],"as":"region_s"}},
{"$unwind":"$region_s"},
{"$match":{"$or":[{"p_comment":{"$regex":{"$regex":"^egul","$options":""}}}]}}],"cursor":{},"idx":81}
```
### >>> Subjoin 81-0
```
db.part.aggregate(EJSON.deserialize(
[
{"$match":{"p_comment":{"$regex":"^egul"}}}]
));
```
Subjoin plan:
```
COLLSCAN: plan_stability_subjoin_cardinality_md.part {"p_comment":{"$regex":"^egul"}}
```
Estimated cardinality: 100  
Actual cardinality: 110  
Orders of magnitude: 0

---
### >>> Subjoin 81-1
```
db.part.aggregate(EJSON.deserialize(
[
{"$match":{"p_comment":{"$regex":"^egul"}}},
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","as":"partsupp","pipeline":[
{"$match":{"$and":[{"$and":[{"$or":[{"ps_supplycost":{"$lt":941.7}},{"ps_availqty":{"$gt":5124}},{"ps_availqty":{"$gte":756}}]},{"ps_availqty":{"$lte":854}},{"ps_comment":{"$regex":"^ "}}]},{}]}}]}},
{"$unwind":"$partsupp"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$partsupp"]}}}]
));
```
Subjoin plan:
```
INLJ p_partkey = ps_partkey
  -> [none] COLLSCAN: plan_stability_subjoin_cardinality_md.part {"p_comment":{"$regex":"^egul"}} 
  -> [partsupp] FETCH: plan_stability_subjoin_cardinality_md.partsupp {"$and":[{"$or":[{"ps_supplycost":{"$lt":941.7}},{"ps_availqty":{"$gt":5124}},{"ps_availqty":{"$gte":756}}]},{"ps_availqty":{"$lte":854}},{"ps_comment":{"$regex":"^ "}}]} 
      -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.partsupp ps_partkey_1
```
Estimated cardinality: 7  
Actual cardinality: 6  
Orders of magnitude: 0

---
### >>> Subjoin 81-2
```
db.part.aggregate(EJSON.deserialize(
[
{"$match":{"p_comment":{"$regex":"^egul"}}},
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","as":"partsupp","pipeline":[
{"$match":{"$and":[{"$and":[{"$or":[{"ps_supplycost":{"$lt":941.7}},{"ps_availqty":{"$gt":5124}},{"ps_availqty":{"$gte":756}}]},{"ps_availqty":{"$lte":854}},{"ps_comment":{"$regex":"^ "}}]},{}]}}]}},
{"$unwind":"$partsupp"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$partsupp"]}}},
{"$lookup":{"from":"supplier","localField":"ps_suppkey","foreignField":"s_suppkey","as":"supplier","pipeline":[
{"$match":{"s_acctbal":{"$gt":4269.56}}}]}},
{"$unwind":"$supplier"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$supplier"]}}}]
));
```
Subjoin plan:
```
HJ partsupp.ps_suppkey = s_suppkey
  -> [none] INLJ p_partkey = ps_partkey
      -> [none] COLLSCAN: plan_stability_subjoin_cardinality_md.part {"p_comment":{"$regex":"^egul"}} 
      -> [partsupp] FETCH: plan_stability_subjoin_cardinality_md.partsupp {"$and":[{"$or":[{"ps_supplycost":{"$lt":941.7}},{"ps_availqty":{"$gt":5124}},{"ps_availqty":{"$gte":756}}]},{"ps_availqty":{"$lte":854}},{"ps_comment":{"$regex":"^ "}}]} 
          -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.partsupp ps_partkey_1
  -> [supplier] COLLSCAN: plan_stability_subjoin_cardinality_md.supplier {"s_acctbal":{"$gt":4269.56}}
```
Estimated cardinality: 4  
Actual cardinality: 4  
Orders of magnitude: 0

---
### >>> Subjoin 81-3
```
db.part.aggregate(EJSON.deserialize(
[
{"$match":{"p_comment":{"$regex":"^egul"}}},
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","as":"partsupp","pipeline":[
{"$match":{"$and":[{"$and":[{"$or":[{"ps_supplycost":{"$lt":941.7}},{"ps_availqty":{"$gt":5124}},{"ps_availqty":{"$gte":756}}]},{"ps_availqty":{"$lte":854}},{"ps_comment":{"$regex":"^ "}}]},{}]}}]}},
{"$unwind":"$partsupp"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$partsupp"]}}},
{"$lookup":{"from":"supplier","localField":"ps_suppkey","foreignField":"s_suppkey","as":"supplier","pipeline":[
{"$match":{"s_acctbal":{"$gt":4269.56}}}]}},
{"$unwind":"$supplier"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$supplier"]}}},
{"$lookup":{"from":"nation","localField":"s_nationkey","foreignField":"n_nationkey","as":"nation","pipeline":[
{"$match":{"n_name":{"$not":{"$in":["FRANCE","RUSSIA"]}}}}]}},
{"$unwind":"$nation"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$nation"]}}}]
));
```
Subjoin plan:
```
HJ supplier.s_nationkey = n_nationkey
  -> [none] HJ partsupp.ps_suppkey = s_suppkey
      -> [none] INLJ p_partkey = ps_partkey
          -> [none] COLLSCAN: plan_stability_subjoin_cardinality_md.part {"p_comment":{"$regex":"^egul"}} 
          -> [partsupp] FETCH: plan_stability_subjoin_cardinality_md.partsupp {"$and":[{"$or":[{"ps_supplycost":{"$lt":941.7}},{"ps_availqty":{"$gt":5124}},{"ps_availqty":{"$gte":756}}]},{"ps_availqty":{"$lte":854}},{"ps_comment":{"$regex":"^ "}}]} 
              -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.partsupp ps_partkey_1
      -> [supplier] COLLSCAN: plan_stability_subjoin_cardinality_md.supplier {"s_acctbal":{"$gt":4269.56}} 
  -> [nation_s] COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"n_name":{"$not":{"$in":["FRANCE","RUSSIA"]}}}
```
Estimated cardinality: 3  
Actual cardinality: 4  
Orders of magnitude: 0

---
### >>> Subjoin 81-4
```
db.part.aggregate(EJSON.deserialize(
[
{"$match":{"p_comment":{"$regex":"^egul"}}},
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","as":"partsupp","pipeline":[
{"$match":{"$and":[{"$and":[{"$or":[{"ps_supplycost":{"$lt":941.7}},{"ps_availqty":{"$gt":5124}},{"ps_availqty":{"$gte":756}}]},{"ps_availqty":{"$lte":854}},{"ps_comment":{"$regex":"^ "}}]},{}]}}]}},
{"$unwind":"$partsupp"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$partsupp"]}}},
{"$lookup":{"from":"supplier","localField":"ps_suppkey","foreignField":"s_suppkey","as":"supplier","pipeline":[
{"$match":{"s_acctbal":{"$gt":4269.56}}}]}},
{"$unwind":"$supplier"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$supplier"]}}},
{"$lookup":{"from":"nation","localField":"s_nationkey","foreignField":"n_nationkey","as":"nation","pipeline":[
{"$match":{"n_name":{"$not":{"$in":["FRANCE","RUSSIA"]}}}}]}},
{"$unwind":"$nation"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$nation"]}}},
{"$lookup":{"from":"region","localField":"n_regionkey","foreignField":"r_regionkey","as":"region","pipeline":[
{"$match":{"$and":[{"r_name":{"$not":{"$eq":"EUROPE"}}},{"r_name":{"$not":{"$eq":"MIDDLE EAST"}}}]}}]}},
{"$unwind":"$region"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$region"]}}}]
));
```
Subjoin plan:
```
HJ r_regionkey = nation_s.n_regionkey
  -> [region_s] COLLSCAN: plan_stability_subjoin_cardinality_md.region {"$and":[{"r_name":{"$not":{"$eq":"EUROPE"}}},{"r_name":{"$not":{"$eq":"MIDDLE EAST"}}}]} 
  -> [none] HJ supplier.s_nationkey = n_nationkey
      -> [none] HJ partsupp.ps_suppkey = s_suppkey
          -> [none] INLJ p_partkey = ps_partkey
              -> [none] COLLSCAN: plan_stability_subjoin_cardinality_md.part {"p_comment":{"$regex":"^egul"}} 
              -> [partsupp] FETCH: plan_stability_subjoin_cardinality_md.partsupp {"$and":[{"$or":[{"ps_supplycost":{"$lt":941.7}},{"ps_availqty":{"$gt":5124}},{"ps_availqty":{"$gte":756}}]},{"ps_availqty":{"$lte":854}},{"ps_comment":{"$regex":"^ "}}]} 
                  -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.partsupp ps_partkey_1
          -> [supplier] COLLSCAN: plan_stability_subjoin_cardinality_md.supplier {"s_acctbal":{"$gt":4269.56}} 
      -> [nation_s] COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"n_name":{"$not":{"$in":["FRANCE","RUSSIA"]}}}
```
Estimated cardinality: 2  
Actual cardinality: 3  
Orders of magnitude: 0

---
## >>> Command idx 82
```
{"aggregate":"supplier","pipeline":[
{"$lookup":{"from":"nation","localField":"s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$nor":[{"n_regionkey":0},{"n_name":{"$nin":["FRANCE","IRAQ","FRANCE"]}},{"n_regionkey":0}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$lookup":{"from":"region","localField":"nation_s.n_regionkey","foreignField":"r_regionkey","pipeline":[
{"$match":{"$or":[{"r_regionkey":4}]}}],"as":"region_s"}},
{"$unwind":"$region_s"},
{"$match":{"$nor":[{"region_s.r_regionkey":0},{"s_acctbal":{"$eq":-707.02}}]}}],"cursor":{},"idx":82}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 83
```
{"aggregate":"lineitem","pipeline":[
{"$lookup":{"from":"orders","localField":"l_orderkey","foreignField":"o_orderkey","pipeline":[
{"$match":{"$and":[{"o_totalprice":{"$lte":42850.94}}]}}],"as":"orders"}},
{"$unwind":"$orders"},
{"$lookup":{"from":"customer","localField":"orders.o_custkey","foreignField":"c_custkey","pipeline":[
{"$match":{"$nor":[{"c_name":"Customer#000008556"}]}},
{"$match":{"$and":[{"c_acctbal":{"$lte":4306.41}}]}}],"as":"customer"}},
{"$unwind":"$customer"},
{"$lookup":{"from":"supplier","localField":"customer.c_nationkey","foreignField":"s_nationkey","pipeline":[
{"$match":{"$and":[{"s_nationkey":0}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$match":{"$nor":[{"customer.c_acctbal":{"$gte":-540.14}},{"l_shipmode":{"$in":["MAIL","AIR","REG AIR","FOB","RAIL"]}}]}}],"cursor":{},"idx":83}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 84
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$or":[{"ps_comment":{"$regex":{"$regex":"^i","$options":""}}}]}},
{"$match":{"$nor":[{"ps_comment":{"$regex":{"$regex":"^ar","$options":""}}},{"ps_availqty":{"$gt":9913}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"supplier","localField":"partsupp.ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$or":[{"s_name":"Supplier#000000813"},{"s_acctbal":{"$lt":1578.18}}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$or":[{"n_name":{"$in":["UNITED STATES","GERMANY"]}},{"n_regionkey":3}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$lookup":{"from":"region","localField":"nation_s.n_regionkey","foreignField":"r_regionkey","pipeline":[
{"$match":{"$nor":[{"r_regionkey":0}]}}],"as":"region_s"}},
{"$unwind":"$region_s"},
{"$match":{"$or":[{"partsupp.ps_availqty":{"$lte":6501}},{"p_name":{"$regex":{"$regex":"^h","$options":""}}}]}}],"cursor":{},"idx":84}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 85
```
{"aggregate":"lineitem","pipeline":[
{"$lookup":{"from":"orders","localField":"l_orderkey","foreignField":"o_orderkey","pipeline":[
{"$match":{"$or":[{"o_totalprice":{"$gte":60817.59}},{"o_clerk":"Clerk#000000632"}]}},
{"$match":{"$or":[{"o_shippriority":{"$gte":0}}]}}],"as":"orders"}},
{"$unwind":"$orders"},
{"$lookup":{"from":"customer","localField":"orders.o_custkey","foreignField":"c_custkey","pipeline":[
{"$match":{"$or":[{"c_name":"Customer#000011428"},{"c_mktsegment":"MACHINERY"}]}},
{"$match":{"$or":[{"c_mktsegment":"AUTOMOBILE"},{"c_name":"Customer#000006656"},{"c_name":"Customer#000006425"},{"c_acctbal":{"$lt":7112.96}}]}}],"as":"customer"}},
{"$unwind":"$customer"},
{"$lookup":{"from":"supplier","localField":"customer.c_nationkey","foreignField":"s_nationkey","pipeline":[
{"$match":{"$and":[{"s_name":"Supplier#000000312"}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$match":{"$or":[{"orders.o_shippriority":{"$lt":0}},{"l_discount":{"$lt":0.02}},{"orders.o_shippriority":{"$lt":0}},{"supplier.o_shippriority":{"$lt":0}}]}}],"cursor":{},"idx":85}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 86
```
{"aggregate":"partsupp","pipeline":[
{"$lookup":{"from":"supplier","localField":"ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$nor":[{"s_nationkey":2}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$and":[{"n_name":{"$nin":["JAPAN","UNITED KINGDOM"]}}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$lookup":{"from":"region","localField":"nation_s.n_regionkey","foreignField":"r_regionkey","pipeline":[
{"$match":{"$or":[{"r_regionkey":4},{"r_regionkey":1}]}}],"as":"region_s"}},
{"$unwind":"$region_s"},
{"$match":{"$and":[{"ps_comment":{"$regex":{"$regex":"^y ","$options":""}}}]}}],"cursor":{},"idx":86}
```
### >>> Subjoin 86-0
```
db.region.aggregate(EJSON.deserialize(
[
{"$match":{"r_regionkey":{"$in":[1,4]}}}]
));
```
Subjoin plan:
```
COLLSCAN: plan_stability_subjoin_cardinality_md.region {"r_regionkey":{"$in":[1,4]}}
```
Estimated cardinality: 2  
Actual cardinality: 2  
Orders of magnitude: 0

---
### >>> Subjoin 86-1
```
db.region.aggregate(EJSON.deserialize(
[
{"$match":{"r_regionkey":{"$in":[1,4]}}},
{"$lookup":{"from":"nation","localField":"r_regionkey","foreignField":"n_regionkey","as":"nation","pipeline":[
{"$match":{"n_name":{"$not":{"$in":["JAPAN","UNITED KINGDOM"]}}}}]}},
{"$unwind":"$nation"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$nation"]}}}]
));
```
Subjoin plan:
```
HJ r_regionkey = n_regionkey
  -> [region_s] COLLSCAN: plan_stability_subjoin_cardinality_md.region {"r_regionkey":{"$in":[1,4]}} 
  -> [nation_s] COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"n_name":{"$not":{"$in":["JAPAN","UNITED KINGDOM"]}}}
```
Estimated cardinality: 9  
Actual cardinality: 10  
Orders of magnitude: 1

---
### >>> Subjoin 86-2
```
db.region.aggregate(EJSON.deserialize(
[
{"$match":{"r_regionkey":{"$in":[1,4]}}},
{"$lookup":{"from":"nation","localField":"r_regionkey","foreignField":"n_regionkey","as":"nation","pipeline":[
{"$match":{"n_name":{"$not":{"$in":["JAPAN","UNITED KINGDOM"]}}}}]}},
{"$unwind":"$nation"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$nation"]}}},
{"$lookup":{"from":"supplier","localField":"n_nationkey","foreignField":"s_nationkey","as":"supplier","pipeline":[
{"$match":{"s_nationkey":{"$not":{"$eq":2}}}}]}},
{"$unwind":"$supplier"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$supplier"]}}}]
));
```
Subjoin plan:
```
HJ nation_s.n_nationkey = s_nationkey
  -> [none] HJ r_regionkey = n_regionkey
      -> [region_s] COLLSCAN: plan_stability_subjoin_cardinality_md.region {"r_regionkey":{"$in":[1,4]}} 
      -> [nation_s] COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"n_name":{"$not":{"$in":["JAPAN","UNITED KINGDOM"]}}} 
  -> [supplier] COLLSCAN: plan_stability_subjoin_cardinality_md.supplier {"s_nationkey":{"$not":{"$eq":2}}}
```
Estimated cardinality: 352  
Actual cardinality: 349  
Orders of magnitude: 0

---
### >>> Subjoin 86-3
```
db.region.aggregate(EJSON.deserialize(
[
{"$match":{"r_regionkey":{"$in":[1,4]}}},
{"$lookup":{"from":"nation","localField":"r_regionkey","foreignField":"n_regionkey","as":"nation","pipeline":[
{"$match":{"n_name":{"$not":{"$in":["JAPAN","UNITED KINGDOM"]}}}}]}},
{"$unwind":"$nation"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$nation"]}}},
{"$lookup":{"from":"supplier","localField":"n_nationkey","foreignField":"s_nationkey","as":"supplier","pipeline":[
{"$match":{"s_nationkey":{"$not":{"$eq":2}}}}]}},
{"$unwind":"$supplier"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$supplier"]}}},
{"$lookup":{"from":"partsupp","localField":"s_suppkey","foreignField":"ps_suppkey","as":"partsupp","pipeline":[
{"$match":{"ps_comment":{"$regex":"^y "}}}]}},
{"$unwind":"$partsupp"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$partsupp"]}}}]
));
```
Subjoin plan:
```
HJ supplier.s_suppkey = ps_suppkey
  -> [none] HJ nation_s.n_nationkey = s_nationkey
      -> [none] HJ r_regionkey = n_regionkey
          -> [region_s] COLLSCAN: plan_stability_subjoin_cardinality_md.region {"r_regionkey":{"$in":[1,4]}} 
          -> [nation_s] COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"n_name":{"$not":{"$in":["JAPAN","UNITED KINGDOM"]}}} 
      -> [supplier] COLLSCAN: plan_stability_subjoin_cardinality_md.supplier {"s_nationkey":{"$not":{"$eq":2}}} 
  -> [none] COLLSCAN: plan_stability_subjoin_cardinality_md.partsupp {"ps_comment":{"$regex":"^y "}}
```
Estimated cardinality: 648  
Actual cardinality: 659  
Orders of magnitude: 0

---
## >>> Command idx 88
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$nor":[{"ps_comment":{"$regex":{"$regex":"^t","$options":""}}}]}},
{"$match":{"$nor":[{"ps_comment":{"$regex":{"$regex":"^ iro","$options":""}}},{"ps_comment":{"$regex":{"$regex":"^s","$options":""}}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"supplier","localField":"partsupp.ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$nor":[{"s_acctbal":{"$lt":6399.78}},{"s_acctbal":{"$lte":747.88}}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$nor":[{"n_name":"CANADA"},{"n_name":"BRAZIL"},{"n_regionkey":1}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$lookup":{"from":"region","localField":"nation_s.n_regionkey","foreignField":"r_regionkey","pipeline":[
{"$match":{"$nor":[{"r_name":"AMERICA"},{"r_regionkey":1},{"r_regionkey":4}]}},
{"$match":{"$and":[{"r_regionkey":3},{"r_regionkey":3}]}},
{"$match":{"$nor":[{"r_name":"MIDDLE EAST"}]}}],"as":"region_s"}},
{"$unwind":"$region_s"},
{"$match":{"$or":[{"partsupp.ps_comment":{"$regex":{"$regex":"^fi","$options":""}}},{"p_name":"dodger papaya purple tomato rose"}]}}],"cursor":{},"idx":88}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 89
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$and":[{"ps_supplycost":{"$gte":969.52}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"supplier","localField":"partsupp.ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$or":[{"s_nationkey":17}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$match":{"$nor":[{"p_type":{"$regex":{"$regex":"^SMALL","$options":""}}},{"p_comment":{"$regex":{"$regex":"^ck","$options":""}}},{"supplier.o_shippriority":{"$gt":0}}]}}],"cursor":{},"idx":89}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 90
```
{"aggregate":"supplier","pipeline":[
{"$lookup":{"from":"nation","localField":"s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$or":[{"n_regionkey":3}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$lookup":{"from":"region","localField":"nation_s.n_regionkey","foreignField":"r_regionkey","pipeline":[
{"$match":{"$nor":[{"r_name":"AFRICA"}]}}],"as":"region_s"}},
{"$unwind":"$region_s"},
{"$match":{"$or":[{"nation_s.n_name":"EGYPT"},{"region_s.r_regionkey":3}]}}],"cursor":{},"idx":90}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 91
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$or":[{"ps_comment":{"$regex":{"$regex":"^ar d","$options":""}}},{"ps_comment":{"$regex":{"$regex":"^ ","$options":""}}}]}},
{"$match":{"$or":[{"ps_availqty":{"$lte":4022}},{"ps_supplycost":{"$eq":705.79}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"lineitem","localField":"partsupp.ps_partkey","foreignField":"l_partkey","pipeline":[
{"$match":{"$or":[{"l_linenumber":{"$gte":3}}]}}],"as":"lineitem"}},
{"$unwind":"$lineitem"},
{"$match":{"$and":[{"p_comment":{"$regex":{"$regex":"^egul","$options":""}}}]}}],"cursor":{},"idx":91}
```
### >>> Subjoin 91-0
```
db.part.aggregate(EJSON.deserialize(
[
{"$match":{"p_comment":{"$regex":"^egul"}}}]
));
```
Subjoin plan:
```
COLLSCAN: plan_stability_subjoin_cardinality_md.part {"p_comment":{"$regex":"^egul"}}
```
Estimated cardinality: 100  
Actual cardinality: 110  
Orders of magnitude: 0

---
### >>> Subjoin 91-1
```
db.part.aggregate(EJSON.deserialize(
[
{"$match":{"p_comment":{"$regex":"^egul"}}},
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","as":"partsupp","pipeline":[
{"$match":{"$and":[{"$and":[{"$or":[{"ps_supplycost":{"$eq":705.79}},{"ps_availqty":{"$lte":4022}}]},{"ps_comment":{"$in":[{"$regex":"^ar d","$options":""},{"$regex":"^ ","$options":""}]}}]},{}]}}]}},
{"$unwind":"$partsupp"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$partsupp"]}}}]
));
```
Subjoin plan:
```
INLJ p_partkey = ps_partkey
  -> [none] COLLSCAN: plan_stability_subjoin_cardinality_md.part {"p_comment":{"$regex":"^egul"}} 
  -> [partsupp] FETCH: plan_stability_subjoin_cardinality_md.partsupp {"$and":[{"$or":[{"ps_supplycost":{"$eq":705.79}},{"ps_availqty":{"$lte":4022}}]},{"ps_comment":{"$in":[{"$regex":"^ar d","$options":""},{"$regex":"^ ","$options":""}]}}]} 
      -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.partsupp ps_partkey_1
```
Estimated cardinality: 23  
Actual cardinality: 23  
Orders of magnitude: 0

---
### >>> Subjoin 91-2
```
db.part.aggregate(EJSON.deserialize(
[
{"$match":{"p_comment":{"$regex":"^egul"}}},
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","as":"partsupp","pipeline":[
{"$match":{"$and":[{"$and":[{"$or":[{"ps_supplycost":{"$eq":705.79}},{"ps_availqty":{"$lte":4022}}]},{"ps_comment":{"$in":[{"$regex":"^ar d","$options":""},{"$regex":"^ ","$options":""}]}}]},{}]}}]}},
{"$unwind":"$partsupp"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$partsupp"]}}},
{"$lookup":{"from":"lineitem","localField":"p_partkey","foreignField":"l_partkey","as":"lineitem","pipeline":[
{"$match":{"$and":[{"l_linenumber":{"$gte":3}},{}]}}]}},
{"$unwind":"$lineitem"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$lineitem"]}}}]
));
```
Subjoin plan:
```
INLJ partsupp.ps_partkey = l_partkey, p_partkey = l_partkey
  -> [none] INLJ p_partkey = ps_partkey
      -> [none] COLLSCAN: plan_stability_subjoin_cardinality_md.part {"p_comment":{"$regex":"^egul"}} 
      -> [partsupp] FETCH: plan_stability_subjoin_cardinality_md.partsupp {"$and":[{"$or":[{"ps_supplycost":{"$eq":705.79}},{"ps_availqty":{"$lte":4022}}]},{"ps_comment":{"$in":[{"$regex":"^ar d","$options":""},{"$regex":"^ ","$options":""}]}}]} 
          -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.partsupp ps_partkey_1
  -> [lineitem] FETCH: plan_stability_subjoin_cardinality_md.lineitem {"l_linenumber":{"$gte":3}} 
      -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.lineitem l_partkey_1
```
Estimated cardinality: 28844  
Actual cardinality: 384  
Orders of magnitude: 2
> [!WARNING]
> Estimate discrepancy is more than 2 orders of magnitude.

---
## >>> Command idx 92
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$or":[{"ps_comment":{"$regex":{"$regex":"^ nag","$options":""}}},{"ps_availqty":{"$eq":6218}},{"ps_comment":{"$regex":{"$regex":"^t ","$options":""}}}]}},
{"$match":{"$nor":[{"ps_availqty":{"$gt":7214}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"lineitem","localField":"partsupp.ps_partkey","foreignField":"l_partkey","pipeline":[
{"$match":{"$or":[{"l_shipmode":{"$in":["RAIL","REG AIR"]}}]}}],"as":"lineitem"}},
{"$unwind":"$lineitem"},
{"$match":{"$or":[{"lineitem.l_linenumber":{"$gte":5}}]}}],"cursor":{},"idx":92}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 93
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$nor":[{"ps_supplycost":{"$lt":354.85}},{"ps_comment":{"$regex":{"$regex":"^ pa","$options":""}}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"supplier","localField":"partsupp.ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$and":[{"s_acctbal":{"$lte":-609.59}}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$match":{"$nor":[{"p_size":{"$lte":28}}]}}],"cursor":{},"idx":93}
```
### >>> Subjoin 93-0
```
db.supplier.aggregate(EJSON.deserialize(
[
{"$match":{"s_acctbal":{"$lte":-609.59}}}]
));
```
Subjoin plan:
```
COLLSCAN: plan_stability_subjoin_cardinality_md.supplier {"s_acctbal":{"$lte":-609.59}}
```
Estimated cardinality: 34  
Actual cardinality: 34  
Orders of magnitude: 0

---
### >>> Subjoin 93-1
```
db.supplier.aggregate(EJSON.deserialize(
[
{"$match":{"s_acctbal":{"$lte":-609.59}}},
{"$lookup":{"from":"partsupp","localField":"s_suppkey","foreignField":"ps_suppkey","as":"partsupp","pipeline":[
{"$match":{"$and":[{"$nor":[{"ps_supplycost":{"$lt":354.85}},{"ps_comment":{"$regex":"^ pa"}}]},{}]}}]}},
{"$unwind":"$partsupp"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$partsupp"]}}}]
));
```
Subjoin plan:
```
INLJ s_suppkey = ps_suppkey
  -> [supplier] COLLSCAN: plan_stability_subjoin_cardinality_md.supplier {"s_acctbal":{"$lte":-609.59}} 
  -> [partsupp] FETCH: plan_stability_subjoin_cardinality_md.partsupp {"$nor":[{"ps_supplycost":{"$lt":354.85}},{"ps_comment":{"$regex":"^ pa"}}]} 
      -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.partsupp ps_suppkey_1
```
Estimated cardinality: 1790  
Actual cardinality: 1782  
Orders of magnitude: 0

---
### >>> Subjoin 93-2
```
db.supplier.aggregate(EJSON.deserialize(
[
{"$match":{"s_acctbal":{"$lte":-609.59}}},
{"$lookup":{"from":"partsupp","localField":"s_suppkey","foreignField":"ps_suppkey","as":"partsupp","pipeline":[
{"$match":{"$and":[{"$nor":[{"ps_supplycost":{"$lt":354.85}},{"ps_comment":{"$regex":"^ pa"}}]},{}]}}]}},
{"$unwind":"$partsupp"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$partsupp"]}}},
{"$lookup":{"from":"part","localField":"ps_partkey","foreignField":"p_partkey","as":"part","pipeline":[
{"$match":{"p_size":{"$not":{"$lte":28}}}}]}},
{"$unwind":"$part"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$part"]}}}]
));
```
Subjoin plan:
```
HJ partsupp.ps_partkey = p_partkey
  -> [none] INLJ s_suppkey = ps_suppkey
      -> [supplier] COLLSCAN: plan_stability_subjoin_cardinality_md.supplier {"s_acctbal":{"$lte":-609.59}} 
      -> [partsupp] FETCH: plan_stability_subjoin_cardinality_md.partsupp {"$nor":[{"ps_supplycost":{"$lt":354.85}},{"ps_comment":{"$regex":"^ pa"}}]} 
          -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.partsupp ps_suppkey_1
  -> [none] COLLSCAN: plan_stability_subjoin_cardinality_md.part {"p_size":{"$not":{"$lte":28}}}
```
Estimated cardinality: 750  
Actual cardinality: 773  
Orders of magnitude: 0

---
## >>> Command idx 94
```
{"aggregate":"partsupp","pipeline":[
{"$lookup":{"from":"supplier","localField":"ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$nor":[{"s_acctbal":{"$lt":6113.96}},{"s_acctbal":{"$eq":4111.07}},{"s_acctbal":{"$lte":9537.73}}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$or":[{"n_regionkey":1},{"n_name":"MOROCCO"},{"n_name":{"$nin":["ARGENTINA","EGYPT"]}}]}},
{"$match":{"$and":[{"n_name":{"$in":["CANADA","MOROCCO"]}}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$match":{"$or":[{"nation_s.n_regionkey":0}]}}],"cursor":{},"idx":94}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 95
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$and":[{"ps_comment":{"$regex":{"$regex":"^s","$options":""}}}]}},
{"$match":{"$or":[{"ps_availqty":{"$gt":2302}},{"ps_supplycost":{"$lt":37.64}},{"ps_supplycost":{"$gt":247.77}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"supplier","localField":"partsupp.ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$nor":[{"s_acctbal":{"$lt":5046.81}},{"s_acctbal":{"$lte":4663.08}},{"s_nationkey":21}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$or":[{"n_name":{"$nin":["ETHIOPIA","ALGERIA"]}},{"n_regionkey":3}]}},
{"$match":{"$nor":[{"n_name":{"$nin":["ARGENTINA","UNITED STATES","IRAQ","GERMANY"]}},{"n_name":{"$in":["RUSSIA","EGYPT"]}}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$lookup":{"from":"region","localField":"nation_s.n_regionkey","foreignField":"r_regionkey","pipeline":[
{"$match":{"$nor":[{"r_regionkey":4}]}}],"as":"region_s"}},
{"$unwind":"$region_s"},
{"$match":{"$or":[{"partsupp.ps_supplycost":{"$lt":146.47}},{"p_container":{"$nin":["JUMBO JAR","JUMBO BOX"]}},{"nation_s.n_name":{"$in":["ETHIOPIA","ROMANIA","FRANCE"]}}]}}],"cursor":{},"idx":95}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 96
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$or":[{"ps_comment":{"$regex":{"$regex":"^ ","$options":""}}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"supplier","localField":"partsupp.ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$nor":[{"s_nationkey":7},{"s_name":"Supplier#000000326"}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$nor":[{"n_name":"BRAZIL"}]}},
{"$match":{"$or":[{"n_regionkey":3}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$lookup":{"from":"region","localField":"nation_s.n_regionkey","foreignField":"r_regionkey","pipeline":[
{"$match":{"$nor":[{"r_regionkey":0},{"r_name":"AFRICA"}]}}],"as":"region_s"}},
{"$unwind":"$region_s"},
{"$match":{"$and":[{"p_mfgr":{"$nin":["Manufacturer#2","Manufacturer#3","Manufacturer#1","Manufacturer#1"]}}]}}],"cursor":{},"idx":96}
```
### >>> Subjoin 96-0
```
db.region.aggregate(EJSON.deserialize(
[
{"$match":{"$nor":[{"r_name":{"$eq":"AFRICA"}},{"r_regionkey":{"$eq":0}}]}}]
));
```
Subjoin plan:
```
COLLSCAN: plan_stability_subjoin_cardinality_md.region {"$nor":[{"r_name":{"$eq":"AFRICA"}},{"r_regionkey":{"$eq":0}}]}
```
Estimated cardinality: 4  
Actual cardinality: 4  
Orders of magnitude: 0

---
### >>> Subjoin 96-1
```
db.region.aggregate(EJSON.deserialize(
[
{"$match":{"$nor":[{"r_name":{"$eq":"AFRICA"}},{"r_regionkey":{"$eq":0}}]}},
{"$lookup":{"from":"nation","localField":"r_regionkey","foreignField":"n_regionkey","as":"nation","pipeline":[
{"$match":{"$and":[{"$and":[{"n_regionkey":{"$eq":3}},{"n_name":{"$not":{"$eq":"BRAZIL"}}}]},{}]}}]}},
{"$unwind":"$nation"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$nation"]}}}]
));
```
Subjoin plan:
```
INLJ r_regionkey = n_regionkey
  -> [region_s] COLLSCAN: plan_stability_subjoin_cardinality_md.region {"$nor":[{"r_name":{"$eq":"AFRICA"}},{"r_regionkey":{"$eq":0}}]} 
  -> [nation_s] FETCH: plan_stability_subjoin_cardinality_md.nation {"$and":[{"n_regionkey":{"$eq":3}},{"n_name":{"$not":{"$eq":"BRAZIL"}}}]} 
      -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.nation n_regionkey_1
```
Estimated cardinality: 4  
Actual cardinality: 5  
Orders of magnitude: 0

---
### >>> Subjoin 96-2
```
db.region.aggregate(EJSON.deserialize(
[
{"$match":{"$nor":[{"r_name":{"$eq":"AFRICA"}},{"r_regionkey":{"$eq":0}}]}},
{"$lookup":{"from":"nation","localField":"r_regionkey","foreignField":"n_regionkey","as":"nation","pipeline":[
{"$match":{"$and":[{"$and":[{"n_regionkey":{"$eq":3}},{"n_name":{"$not":{"$eq":"BRAZIL"}}}]},{}]}}]}},
{"$unwind":"$nation"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$nation"]}}},
{"$lookup":{"from":"supplier","localField":"n_nationkey","foreignField":"s_nationkey","as":"supplier","pipeline":[
{"$match":{"$nor":[{"s_name":{"$eq":"Supplier#000000326"}},{"s_nationkey":{"$eq":7}}]}}]}},
{"$unwind":"$supplier"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$supplier"]}}}]
));
```
Subjoin plan:
```
HJ nation_s.n_nationkey = s_nationkey
  -> [none] INLJ r_regionkey = n_regionkey
      -> [region_s] COLLSCAN: plan_stability_subjoin_cardinality_md.region {"$nor":[{"r_name":{"$eq":"AFRICA"}},{"r_regionkey":{"$eq":0}}]} 
      -> [nation_s] FETCH: plan_stability_subjoin_cardinality_md.nation {"$and":[{"n_regionkey":{"$eq":3}},{"n_name":{"$not":{"$eq":"BRAZIL"}}}]} 
          -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.nation n_regionkey_1
  -> [supplier] COLLSCAN: plan_stability_subjoin_cardinality_md.supplier {"$nor":[{"s_name":{"$eq":"Supplier#000000326"}},{"s_nationkey":{"$eq":7}}]}
```
Estimated cardinality: 152  
Actual cardinality: 154  
Orders of magnitude: 0

---
### >>> Subjoin 96-3
```
db.region.aggregate(EJSON.deserialize(
[
{"$match":{"$nor":[{"r_name":{"$eq":"AFRICA"}},{"r_regionkey":{"$eq":0}}]}},
{"$lookup":{"from":"nation","localField":"r_regionkey","foreignField":"n_regionkey","as":"nation","pipeline":[
{"$match":{"$and":[{"$and":[{"n_regionkey":{"$eq":3}},{"n_name":{"$not":{"$eq":"BRAZIL"}}}]},{}]}}]}},
{"$unwind":"$nation"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$nation"]}}},
{"$lookup":{"from":"supplier","localField":"n_nationkey","foreignField":"s_nationkey","as":"supplier","pipeline":[
{"$match":{"$nor":[{"s_name":{"$eq":"Supplier#000000326"}},{"s_nationkey":{"$eq":7}}]}}]}},
{"$unwind":"$supplier"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$supplier"]}}},
{"$lookup":{"from":"partsupp","localField":"s_suppkey","foreignField":"ps_suppkey","as":"partsupp","pipeline":[
{"$match":{"$and":[{"ps_comment":{"$regex":"^ "}},{}]}}]}},
{"$unwind":"$partsupp"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$partsupp"]}}}]
));
```
Subjoin plan:
```
INLJ supplier.s_suppkey = ps_suppkey
  -> [none] HJ nation_s.n_nationkey = s_nationkey
      -> [none] INLJ r_regionkey = n_regionkey
          -> [region_s] COLLSCAN: plan_stability_subjoin_cardinality_md.region {"$nor":[{"r_name":{"$eq":"AFRICA"}},{"r_regionkey":{"$eq":0}}]} 
          -> [nation_s] FETCH: plan_stability_subjoin_cardinality_md.nation {"$and":[{"n_regionkey":{"$eq":3}},{"n_name":{"$not":{"$eq":"BRAZIL"}}}]} 
              -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.nation n_regionkey_1
      -> [supplier] COLLSCAN: plan_stability_subjoin_cardinality_md.supplier {"$nor":[{"s_name":{"$eq":"Supplier#000000326"}},{"s_nationkey":{"$eq":7}}]} 
  -> [partsupp] FETCH: plan_stability_subjoin_cardinality_md.partsupp {"ps_comment":{"$regex":"^ "}} 
      -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.partsupp ps_suppkey_1
```
Estimated cardinality: 1739  
Actual cardinality: 1613  
Orders of magnitude: 0

---
### >>> Subjoin 96-4
```
db.region.aggregate(EJSON.deserialize(
[
{"$match":{"$nor":[{"r_name":{"$eq":"AFRICA"}},{"r_regionkey":{"$eq":0}}]}},
{"$lookup":{"from":"nation","localField":"r_regionkey","foreignField":"n_regionkey","as":"nation","pipeline":[
{"$match":{"$and":[{"$and":[{"n_regionkey":{"$eq":3}},{"n_name":{"$not":{"$eq":"BRAZIL"}}}]},{}]}}]}},
{"$unwind":"$nation"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$nation"]}}},
{"$lookup":{"from":"supplier","localField":"n_nationkey","foreignField":"s_nationkey","as":"supplier","pipeline":[
{"$match":{"$nor":[{"s_name":{"$eq":"Supplier#000000326"}},{"s_nationkey":{"$eq":7}}]}}]}},
{"$unwind":"$supplier"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$supplier"]}}},
{"$lookup":{"from":"partsupp","localField":"s_suppkey","foreignField":"ps_suppkey","as":"partsupp","pipeline":[
{"$match":{"$and":[{"ps_comment":{"$regex":"^ "}},{}]}}]}},
{"$unwind":"$partsupp"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$partsupp"]}}},
{"$lookup":{"from":"part","localField":"ps_partkey","foreignField":"p_partkey","as":"part","pipeline":[
{"$match":{"p_mfgr":{"$not":{"$in":["Manufacturer#1","Manufacturer#2","Manufacturer#3"]}}}}]}},
{"$unwind":"$part"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$part"]}}}]
));
```
Subjoin plan:
```
HJ partsupp.ps_partkey = p_partkey
  -> [none] INLJ supplier.s_suppkey = ps_suppkey
      -> [none] HJ nation_s.n_nationkey = s_nationkey
          -> [none] INLJ r_regionkey = n_regionkey
              -> [region_s] COLLSCAN: plan_stability_subjoin_cardinality_md.region {"$nor":[{"r_name":{"$eq":"AFRICA"}},{"r_regionkey":{"$eq":0}}]} 
              -> [nation_s] FETCH: plan_stability_subjoin_cardinality_md.nation {"$and":[{"n_regionkey":{"$eq":3}},{"n_name":{"$not":{"$eq":"BRAZIL"}}}]} 
                  -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.nation n_regionkey_1
          -> [supplier] COLLSCAN: plan_stability_subjoin_cardinality_md.supplier {"$nor":[{"s_name":{"$eq":"Supplier#000000326"}},{"s_nationkey":{"$eq":7}}]} 
      -> [partsupp] FETCH: plan_stability_subjoin_cardinality_md.partsupp {"ps_comment":{"$regex":"^ "}} 
          -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.partsupp ps_suppkey_1
  -> [none] COLLSCAN: plan_stability_subjoin_cardinality_md.part {"p_mfgr":{"$not":{"$in":["Manufacturer#1","Manufacturer#2","Manufacturer#3"]}}}
```
Estimated cardinality: 676  
Actual cardinality: 665  
Orders of magnitude: 0

---
## >>> Command idx 97
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$nor":[{"ps_supplycost":{"$lt":836.01}}]}},
{"$match":{"$nor":[{"ps_availqty":{"$lte":5069}},{"ps_availqty":{"$lt":1843}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"supplier","localField":"partsupp.ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$and":[{"s_nationkey":2}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$match":{"$nor":[{"p_mfgr":"Manufacturer#3"},{"supplier.s_acctbal":{"$lte":-609.59}}]}}],"cursor":{},"idx":97}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 98
```
{"aggregate":"supplier","pipeline":[
{"$lookup":{"from":"nation","localField":"s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$and":[{"n_name":"UNITED KINGDOM"}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$lookup":{"from":"region","localField":"nation_s.n_regionkey","foreignField":"r_regionkey","pipeline":[
{"$match":{"$or":[{"r_name":"EUROPE"},{"r_name":"AMERICA"}]}}],"as":"region_s"}},
{"$unwind":"$region_s"},
{"$match":{"$or":[{"s_name":"Supplier#000000928"},{"nation_s.n_regionkey":3}]}}],"cursor":{},"idx":98}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 99
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$or":[{"ps_comment":{"$regex":{"$regex":"^no","$options":""}}},{"ps_supplycost":{"$eq":675.61}},{"ps_comment":{"$regex":{"$regex":"^r","$options":""}}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"supplier","localField":"partsupp.ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$nor":[{"s_acctbal":{"$gt":7162.15}}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$or":[{"n_name":"CHINA"}]}},
{"$match":{"$or":[{"n_name":"ROMANIA"},{"n_name":{"$nin":["KENYA","SAUDI ARABIA"]}},{"n_regionkey":2},{"n_regionkey":0}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$match":{"$nor":[{"nation_s.n_name":"INDIA"},{"nation_s.n_name":{"$in":["BRAZIL","KENYA"]}},{"p_type":{"$regex":{"$regex":"^ECONOMY","$options":""}}}]}}],"cursor":{},"idx":99}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 100
```
{"aggregate":"supplier","pipeline":[
{"$lookup":{"from":"nation","localField":"s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$or":[{"n_regionkey":3},{"n_name":"IRAN"},{"n_regionkey":1}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$lookup":{"from":"region","localField":"nation_s.n_regionkey","foreignField":"r_regionkey","pipeline":[
{"$match":{"$nor":[{"r_name":"EUROPE"},{"r_regionkey":2},{"r_name":"AFRICA"}]}}],"as":"region_s"}},
{"$unwind":"$region_s"},
{"$match":{"$or":[{"s_acctbal":{"$gt":8924.02}}]}}],"cursor":{},"idx":100}
```
### >>> Subjoin 100-0
```
db.region.aggregate(EJSON.deserialize(
[
{"$match":{"$nor":[{"r_name":{"$eq":"EUROPE"}},{"r_name":{"$eq":"AFRICA"}},{"r_regionkey":{"$eq":2}}]}}]
));
```
Subjoin plan:
```
COLLSCAN: plan_stability_subjoin_cardinality_md.region {"$nor":[{"r_name":{"$eq":"EUROPE"}},{"r_name":{"$eq":"AFRICA"}},{"r_regionkey":{"$eq":2}}]}
```
Estimated cardinality: 2  
Actual cardinality: 2  
Orders of magnitude: 0

---
### >>> Subjoin 100-1
```
db.region.aggregate(EJSON.deserialize(
[
{"$match":{"$nor":[{"r_name":{"$eq":"EUROPE"}},{"r_name":{"$eq":"AFRICA"}},{"r_regionkey":{"$eq":2}}]}},
{"$lookup":{"from":"nation","localField":"r_regionkey","foreignField":"n_regionkey","as":"nation","pipeline":[
{"$match":{"$or":[{"n_name":{"$eq":"IRAN"}},{"n_regionkey":{"$in":[1,3]}}]}}]}},
{"$unwind":"$nation"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$nation"]}}}]
));
```
Subjoin plan:
```
HJ r_regionkey = n_regionkey
  -> [region_s] COLLSCAN: plan_stability_subjoin_cardinality_md.region {"$nor":[{"r_name":{"$eq":"EUROPE"}},{"r_name":{"$eq":"AFRICA"}},{"r_regionkey":{"$eq":2}}]} 
  -> [nation_s] COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"$or":[{"n_name":{"$eq":"IRAN"}},{"n_regionkey":{"$in":[1,3]}}]}
```
Estimated cardinality: 4  
Actual cardinality: 6  
Orders of magnitude: 0

---
### >>> Subjoin 100-2
```
db.region.aggregate(EJSON.deserialize(
[
{"$match":{"$nor":[{"r_name":{"$eq":"EUROPE"}},{"r_name":{"$eq":"AFRICA"}},{"r_regionkey":{"$eq":2}}]}},
{"$lookup":{"from":"nation","localField":"r_regionkey","foreignField":"n_regionkey","as":"nation","pipeline":[
{"$match":{"$or":[{"n_name":{"$eq":"IRAN"}},{"n_regionkey":{"$in":[1,3]}}]}}]}},
{"$unwind":"$nation"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$nation"]}}},
{"$lookup":{"from":"supplier","localField":"n_nationkey","foreignField":"s_nationkey","as":"supplier","pipeline":[
{"$match":{"s_acctbal":{"$gt":8924.02}}}]}},
{"$unwind":"$supplier"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$supplier"]}}}]
));
```
Subjoin plan:
```
HJ nation_s.n_nationkey = s_nationkey
  -> [none] HJ r_regionkey = n_regionkey
      -> [region_s] COLLSCAN: plan_stability_subjoin_cardinality_md.region {"$nor":[{"r_name":{"$eq":"EUROPE"}},{"r_name":{"$eq":"AFRICA"}},{"r_regionkey":{"$eq":2}}]} 
      -> [nation_s] COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"$or":[{"n_name":{"$eq":"IRAN"}},{"n_regionkey":{"$in":[1,3]}}]} 
  -> [none] COLLSCAN: plan_stability_subjoin_cardinality_md.supplier {"s_acctbal":{"$gt":8924.02}}
```
Estimated cardinality: 19  
Actual cardinality: 29  
Orders of magnitude: 0

---
## >>> Command idx 101
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$and":[{"ps_availqty":{"$lte":298}}]}},
{"$match":{"$or":[{"ps_supplycost":{"$lt":707.48}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"supplier","localField":"partsupp.ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$nor":[{"s_name":"Supplier#000000547"},{"s_name":"Supplier#000000906"}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$or":[{"n_regionkey":3},{"n_regionkey":2}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$match":{"$and":[{"p_comment":{"$regex":{"$regex":"^ions","$options":""}}}]}}],"cursor":{},"idx":101}
```
### >>> Subjoin 101-0
```
db.part.aggregate(EJSON.deserialize(
[
{"$match":{"p_comment":{"$regex":"^ions"}}}]
));
```
Subjoin plan:
```
COLLSCAN: plan_stability_subjoin_cardinality_md.part {"p_comment":{"$regex":"^ions"}}
```
Estimated cardinality: 40  
Actual cardinality: 38  
Orders of magnitude: 0

---
### >>> Subjoin 101-1
```
db.part.aggregate(EJSON.deserialize(
[
{"$match":{"p_comment":{"$regex":"^ions"}}},
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","as":"partsupp","pipeline":[
{"$match":{"$and":[{"$and":[{"ps_availqty":{"$lte":298}},{"ps_supplycost":{"$lt":707.48}}]},{}]}}]}},
{"$unwind":"$partsupp"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$partsupp"]}}}]
));
```
Subjoin plan:
```
INLJ p_partkey = ps_partkey
  -> [none] COLLSCAN: plan_stability_subjoin_cardinality_md.part {"p_comment":{"$regex":"^ions"}} 
  -> [partsupp] FETCH: plan_stability_subjoin_cardinality_md.partsupp {"$and":[{"ps_availqty":{"$lte":298}},{"ps_supplycost":{"$lt":707.48}}]} 
      -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.partsupp ps_partkey_1
```
Estimated cardinality: 3  
Actual cardinality: 5  
Orders of magnitude: 0

---
### >>> Subjoin 101-2
```
db.part.aggregate(EJSON.deserialize(
[
{"$match":{"p_comment":{"$regex":"^ions"}}},
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","as":"partsupp","pipeline":[
{"$match":{"$and":[{"$and":[{"ps_availqty":{"$lte":298}},{"ps_supplycost":{"$lt":707.48}}]},{}]}}]}},
{"$unwind":"$partsupp"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$partsupp"]}}},
{"$lookup":{"from":"supplier","localField":"ps_suppkey","foreignField":"s_suppkey","as":"supplier","pipeline":[
{"$match":{"$and":[{"$nor":[{"s_name":{"$eq":"Supplier#000000547"}},{"s_name":{"$eq":"Supplier#000000906"}}]},{}]}}]}},
{"$unwind":"$supplier"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$supplier"]}}}]
));
```
Subjoin plan:
```
INLJ partsupp.ps_suppkey = s_suppkey
  -> [none] INLJ p_partkey = ps_partkey
      -> [none] COLLSCAN: plan_stability_subjoin_cardinality_md.part {"p_comment":{"$regex":"^ions"}} 
      -> [partsupp] FETCH: plan_stability_subjoin_cardinality_md.partsupp {"$and":[{"ps_availqty":{"$lte":298}},{"ps_supplycost":{"$lt":707.48}}]} 
          -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.partsupp ps_partkey_1
  -> [supplier] FETCH: plan_stability_subjoin_cardinality_md.supplier {"$nor":[{"s_name":{"$eq":"Supplier#000000547"}},{"s_name":{"$eq":"Supplier#000000906"}}]} 
      -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.supplier s_suppkey_1
```
Estimated cardinality: 3  
Actual cardinality: 5  
Orders of magnitude: 0

---
### >>> Subjoin 101-3
```
db.part.aggregate(EJSON.deserialize(
[
{"$match":{"p_comment":{"$regex":"^ions"}}},
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","as":"partsupp","pipeline":[
{"$match":{"$and":[{"$and":[{"ps_availqty":{"$lte":298}},{"ps_supplycost":{"$lt":707.48}}]},{}]}}]}},
{"$unwind":"$partsupp"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$partsupp"]}}},
{"$lookup":{"from":"supplier","localField":"ps_suppkey","foreignField":"s_suppkey","as":"supplier","pipeline":[
{"$match":{"$and":[{"$nor":[{"s_name":{"$eq":"Supplier#000000547"}},{"s_name":{"$eq":"Supplier#000000906"}}]},{}]}}]}},
{"$unwind":"$supplier"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$supplier"]}}},
{"$lookup":{"from":"nation","localField":"s_nationkey","foreignField":"n_nationkey","as":"nation","pipeline":[
{"$match":{"n_regionkey":{"$in":[2,3]}}}]}},
{"$unwind":"$nation"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$nation"]}}}]
));
```
Subjoin plan:
```
HJ supplier.s_nationkey = n_nationkey
  -> [none] INLJ partsupp.ps_suppkey = s_suppkey
      -> [none] INLJ p_partkey = ps_partkey
          -> [none] COLLSCAN: plan_stability_subjoin_cardinality_md.part {"p_comment":{"$regex":"^ions"}} 
          -> [partsupp] FETCH: plan_stability_subjoin_cardinality_md.partsupp {"$and":[{"ps_availqty":{"$lte":298}},{"ps_supplycost":{"$lt":707.48}}]} 
              -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.partsupp ps_partkey_1
      -> [supplier] FETCH: plan_stability_subjoin_cardinality_md.supplier {"$nor":[{"s_name":{"$eq":"Supplier#000000547"}},{"s_name":{"$eq":"Supplier#000000906"}}]} 
          -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.supplier s_suppkey_1
  -> [nation_s] COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"n_regionkey":{"$in":[2,3]}}
```
Estimated cardinality: 1  
Actual cardinality: 3  
Orders of magnitude: 0

---
## >>> Command idx 102
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$and":[{"ps_comment":{"$regex":{"$regex":"^p","$options":""}}},{"ps_availqty":{"$gt":5464}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"supplier","localField":"partsupp.ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$nor":[{"s_name":"Supplier#000000290"},{"s_nationkey":5}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$nor":[{"n_regionkey":4},{"n_regionkey":2}]}},
{"$match":{"$nor":[{"n_name":"CANADA"}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$match":{"$nor":[{"p_mfgr":"Manufacturer#1"}]}}],"cursor":{},"idx":102}
```
### >>> Subjoin 102-0
```
db.nation.aggregate(EJSON.deserialize(
[
{"$match":{"$and":[{"n_name":{"$not":{"$eq":"CANADA"}}},{"$nor":[{"n_regionkey":{"$eq":4}},{"n_regionkey":{"$eq":2}}]}]}}]
));
```
Subjoin plan:
```
COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"$and":[{"n_name":{"$not":{"$eq":"CANADA"}}},{"$nor":[{"n_regionkey":{"$eq":4}},{"n_regionkey":{"$eq":2}}]}]}
```
Estimated cardinality: 14  
Actual cardinality: 14  
Orders of magnitude: 0

---
### >>> Subjoin 102-1
```
db.nation.aggregate(EJSON.deserialize(
[
{"$match":{"$and":[{"n_name":{"$not":{"$eq":"CANADA"}}},{"$nor":[{"n_regionkey":{"$eq":4}},{"n_regionkey":{"$eq":2}}]}]}},
{"$lookup":{"from":"supplier","localField":"n_nationkey","foreignField":"s_nationkey","as":"supplier","pipeline":[
{"$match":{"$nor":[{"s_name":{"$eq":"Supplier#000000290"}},{"s_nationkey":{"$eq":5}}]}}]}},
{"$unwind":"$supplier"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$supplier"]}}}]
));
```
Subjoin plan:
```
HJ n_nationkey = s_nationkey
  -> [nation_s] COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"$and":[{"n_name":{"$not":{"$eq":"CANADA"}}},{"$nor":[{"n_regionkey":{"$eq":4}},{"n_regionkey":{"$eq":2}}]}]} 
  -> [supplier] COLLSCAN: plan_stability_subjoin_cardinality_md.supplier {"$nor":[{"s_name":{"$eq":"Supplier#000000290"}},{"s_nationkey":{"$eq":5}}]}
```
Estimated cardinality: 541  
Actual cardinality: 506  
Orders of magnitude: 0

---
### >>> Subjoin 102-2
```
db.nation.aggregate(EJSON.deserialize(
[
{"$match":{"$and":[{"n_name":{"$not":{"$eq":"CANADA"}}},{"$nor":[{"n_regionkey":{"$eq":4}},{"n_regionkey":{"$eq":2}}]}]}},
{"$lookup":{"from":"supplier","localField":"n_nationkey","foreignField":"s_nationkey","as":"supplier","pipeline":[
{"$match":{"$nor":[{"s_name":{"$eq":"Supplier#000000290"}},{"s_nationkey":{"$eq":5}}]}}]}},
{"$unwind":"$supplier"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$supplier"]}}},
{"$lookup":{"from":"partsupp","localField":"s_suppkey","foreignField":"ps_suppkey","as":"partsupp","pipeline":[
{"$match":{"$and":[{"ps_availqty":{"$gt":5464}},{"ps_comment":{"$regex":"^p"}}]}}]}},
{"$unwind":"$partsupp"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$partsupp"]}}}]
));
```
Subjoin plan:
```
HJ supplier.s_suppkey = ps_suppkey
  -> [none] HJ n_nationkey = s_nationkey
      -> [nation_s] COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"$and":[{"n_name":{"$not":{"$eq":"CANADA"}}},{"$nor":[{"n_regionkey":{"$eq":4}},{"n_regionkey":{"$eq":2}}]}]} 
      -> [supplier] COLLSCAN: plan_stability_subjoin_cardinality_md.supplier {"$nor":[{"s_name":{"$eq":"Supplier#000000290"}},{"s_nationkey":{"$eq":5}}]} 
  -> [partsupp] COLLSCAN: plan_stability_subjoin_cardinality_md.partsupp {"$and":[{"ps_availqty":{"$gt":5464}},{"ps_comment":{"$regex":"^p"}}]}
```
Estimated cardinality: 476  
Actual cardinality: 443  
Orders of magnitude: 0

---
### >>> Subjoin 102-3
```
db.nation.aggregate(EJSON.deserialize(
[
{"$match":{"$and":[{"n_name":{"$not":{"$eq":"CANADA"}}},{"$nor":[{"n_regionkey":{"$eq":4}},{"n_regionkey":{"$eq":2}}]}]}},
{"$lookup":{"from":"supplier","localField":"n_nationkey","foreignField":"s_nationkey","as":"supplier","pipeline":[
{"$match":{"$nor":[{"s_name":{"$eq":"Supplier#000000290"}},{"s_nationkey":{"$eq":5}}]}}]}},
{"$unwind":"$supplier"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$supplier"]}}},
{"$lookup":{"from":"partsupp","localField":"s_suppkey","foreignField":"ps_suppkey","as":"partsupp","pipeline":[
{"$match":{"$and":[{"ps_availqty":{"$gt":5464}},{"ps_comment":{"$regex":"^p"}}]}}]}},
{"$unwind":"$partsupp"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$partsupp"]}}},
{"$lookup":{"from":"part","localField":"ps_partkey","foreignField":"p_partkey","as":"part","pipeline":[
{"$match":{"p_mfgr":{"$not":{"$eq":"Manufacturer#1"}}}}]}},
{"$unwind":"$part"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$part"]}}}]
));
```
Subjoin plan:
```
HJ partsupp.ps_partkey = p_partkey
  -> [none] HJ supplier.s_suppkey = ps_suppkey
      -> [none] HJ n_nationkey = s_nationkey
          -> [nation_s] COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"$and":[{"n_name":{"$not":{"$eq":"CANADA"}}},{"$nor":[{"n_regionkey":{"$eq":4}},{"n_regionkey":{"$eq":2}}]}]} 
          -> [supplier] COLLSCAN: plan_stability_subjoin_cardinality_md.supplier {"$nor":[{"s_name":{"$eq":"Supplier#000000290"}},{"s_nationkey":{"$eq":5}}]} 
      -> [partsupp] COLLSCAN: plan_stability_subjoin_cardinality_md.partsupp {"$and":[{"ps_availqty":{"$gt":5464}},{"ps_comment":{"$regex":"^p"}}]} 
  -> [none] COLLSCAN: plan_stability_subjoin_cardinality_md.part {"p_mfgr":{"$not":{"$eq":"Manufacturer#1"}}}
```
Estimated cardinality: 384  
Actual cardinality: 359  
Orders of magnitude: 0

---
## >>> Command idx 103
```
{"aggregate":"supplier","pipeline":[
{"$lookup":{"from":"nation","localField":"s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$nor":[{"n_name":"ALGERIA"}]}},
{"$match":{"$or":[{"n_name":{"$nin":["MOZAMBIQUE","CANADA"]}},{"n_name":"MOZAMBIQUE"},{"n_regionkey":3},{"n_name":"INDIA"},{"n_name":"KENYA"}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$lookup":{"from":"region","localField":"nation_s.n_regionkey","foreignField":"r_regionkey","pipeline":[
{"$match":{"$nor":[{"r_regionkey":3}]}}],"as":"region_s"}},
{"$unwind":"$region_s"},
{"$match":{"$and":[{"region_s.r_name":"MIDDLE EAST"},{"s_name":"Supplier#000000926"}]}}],"cursor":{},"idx":103}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 104
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$and":[{"ps_comment":{"$regex":{"$regex":"^bea","$options":""}}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"supplier","localField":"partsupp.ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$nor":[{"s_nationkey":7},{"s_acctbal":{"$eq":7888.41}}]}},
{"$match":{"$or":[{"s_acctbal":{"$gte":7448.46}},{"s_name":"Supplier#000000103"}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$match":{"$nor":[{"supplier.o_shippriority":{"$gte":0}},{"p_type":"MEDIUM ANODIZED NICKEL"},{"supplier.s_name":"Supplier#000000803"},{"p_name":{"$regex":{"$regex":"^b","$options":""}}}]}}],"cursor":{},"idx":104}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 105
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$and":[{"ps_availqty":{"$lt":7155}},{"ps_availqty":{"$gt":4947}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"supplier","localField":"partsupp.ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$or":[{"s_acctbal":{"$lt":-686.97}}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$match":{"$nor":[{"supplier.s_nationkey":4}]}}],"cursor":{},"idx":105}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 106
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$nor":[{"ps_comment":{"$regex":{"$regex":"^ pa","$options":""}}},{"ps_comment":{"$regex":{"$regex":"^tion","$options":""}}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"supplier","localField":"partsupp.ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$nor":[{"s_name":"Supplier#000000439"}]}},
{"$match":{"$or":[{"s_name":"Supplier#000000338"}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$nor":[{"n_name":"FRANCE"}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$lookup":{"from":"region","localField":"nation_s.n_regionkey","foreignField":"r_regionkey","pipeline":[
{"$match":{"$nor":[{"r_regionkey":3}]}},
{"$match":{"$or":[{"r_name":"AMERICA"},{"r_regionkey":1},{"r_name":"EUROPE"},{"r_name":"AFRICA"}]}}],"as":"region_s"}},
{"$unwind":"$region_s"},
{"$match":{"$or":[{"supplier.s_nationkey":2},{"partsupp.ps_supplycost":{"$gte":285.82}}]}}],"cursor":{},"idx":106}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 107
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$nor":[{"ps_comment":{"$regex":{"$regex":"^y","$options":""}}},{"ps_comment":{"$regex":{"$regex":"^t","$options":""}}},{"ps_supplycost":{"$gt":773.39}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"supplier","localField":"partsupp.ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$nor":[{"s_name":"Supplier#000000906"}]}},
{"$match":{"$nor":[{"s_acctbal":{"$lte":10.33}}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$and":[{"n_name":{"$nin":["BRAZIL","ALGERIA"]}}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$lookup":{"from":"region","localField":"nation_s.n_regionkey","foreignField":"r_regionkey","pipeline":[
{"$match":{"$or":[{"r_regionkey":4},{"r_name":"ASIA"}]}}],"as":"region_s"}},
{"$unwind":"$region_s"},
{"$match":{"$or":[{"nation_s.n_regionkey":1},{"p_brand":"Brand#11"}]}}],"cursor":{},"idx":107}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 108
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$or":[{"ps_availqty":{"$lt":6925}}]}},
{"$match":{"$nor":[{"ps_availqty":{"$eq":298}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"supplier","localField":"partsupp.ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$nor":[{"s_name":"Supplier#000000145"},{"s_acctbal":{"$gte":-942.73}}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$match":{"$nor":[{"p_comment":{"$regex":{"$regex":"^s","$options":""}}},{"p_type":"ECONOMY BRUSHED COPPER"}]}}],"cursor":{},"idx":108}
```
### >>> Subjoin 108-0
```
db.supplier.aggregate(EJSON.deserialize(
[
{"$match":{"$nor":[{"s_name":{"$eq":"Supplier#000000145"}},{"s_acctbal":{"$gte":-942.73}}]}}]
));
```
Subjoin plan:
```
COLLSCAN: plan_stability_subjoin_cardinality_md.supplier {"$nor":[{"s_name":{"$eq":"Supplier#000000145"}},{"s_acctbal":{"$gte":-942.73}}]}
```
Estimated cardinality: 4  
Actual cardinality: 4  
Orders of magnitude: 0

---
### >>> Subjoin 108-1
```
db.supplier.aggregate(EJSON.deserialize(
[
{"$match":{"$nor":[{"s_name":{"$eq":"Supplier#000000145"}},{"s_acctbal":{"$gte":-942.73}}]}},
{"$lookup":{"from":"partsupp","localField":"s_suppkey","foreignField":"ps_suppkey","as":"partsupp","pipeline":[
{"$match":{"$and":[{"$and":[{"ps_availqty":{"$lt":6925}},{"ps_availqty":{"$not":{"$eq":298}}}]},{}]}}]}},
{"$unwind":"$partsupp"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$partsupp"]}}}]
));
```
Subjoin plan:
```
INLJ s_suppkey = ps_suppkey
  -> [supplier] COLLSCAN: plan_stability_subjoin_cardinality_md.supplier {"$nor":[{"s_name":{"$eq":"Supplier#000000145"}},{"s_acctbal":{"$gte":-942.73}}]} 
  -> [partsupp] FETCH: plan_stability_subjoin_cardinality_md.partsupp {"$and":[{"ps_availqty":{"$lt":6925}},{"ps_availqty":{"$not":{"$eq":298}}}]} 
      -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.partsupp ps_suppkey_1
```
Estimated cardinality: 223  
Actual cardinality: 217  
Orders of magnitude: 0

---
### >>> Subjoin 108-2
```
db.supplier.aggregate(EJSON.deserialize(
[
{"$match":{"$nor":[{"s_name":{"$eq":"Supplier#000000145"}},{"s_acctbal":{"$gte":-942.73}}]}},
{"$lookup":{"from":"partsupp","localField":"s_suppkey","foreignField":"ps_suppkey","as":"partsupp","pipeline":[
{"$match":{"$and":[{"$and":[{"ps_availqty":{"$lt":6925}},{"ps_availqty":{"$not":{"$eq":298}}}]},{}]}}]}},
{"$unwind":"$partsupp"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$partsupp"]}}},
{"$lookup":{"from":"part","localField":"ps_partkey","foreignField":"p_partkey","as":"part","pipeline":[
{"$match":{"$nor":[{"p_type":{"$eq":"ECONOMY BRUSHED COPPER"}},{"p_comment":{"$regex":"^s"}}]}}]}},
{"$unwind":"$part"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$part"]}}}]
));
```
Subjoin plan:
```
HJ partsupp.ps_partkey = p_partkey
  -> [none] INLJ s_suppkey = ps_suppkey
      -> [supplier] COLLSCAN: plan_stability_subjoin_cardinality_md.supplier {"$nor":[{"s_name":{"$eq":"Supplier#000000145"}},{"s_acctbal":{"$gte":-942.73}}]} 
      -> [partsupp] FETCH: plan_stability_subjoin_cardinality_md.partsupp {"$and":[{"ps_availqty":{"$lt":6925}},{"ps_availqty":{"$not":{"$eq":298}}}]} 
          -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.partsupp ps_suppkey_1
  -> [none] COLLSCAN: plan_stability_subjoin_cardinality_md.part {"$nor":[{"p_type":{"$eq":"ECONOMY BRUSHED COPPER"}},{"p_comment":{"$regex":"^s"}}]}
```
Estimated cardinality: 205  
Actual cardinality: 199  
Orders of magnitude: 0

---
## >>> Command idx 109
```
{"aggregate":"partsupp","pipeline":[
{"$lookup":{"from":"supplier","localField":"ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$nor":[{"s_acctbal":{"$gte":6835.16}},{"s_name":"Supplier#000000832"},{"s_nationkey":19}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$nor":[{"n_name":{"$nin":["ALGERIA","UNITED STATES"]}},{"n_regionkey":3}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$lookup":{"from":"region","localField":"nation_s.n_regionkey","foreignField":"r_regionkey","pipeline":[
{"$match":{"$or":[{"r_regionkey":0},{"r_name":"AFRICA"}]}}],"as":"region_s"}},
{"$unwind":"$region_s"},
{"$match":{"$and":[{"ps_comment":{"$regex":{"$regex":"^slyl","$options":""}}}]}}],"cursor":{},"idx":109}
```
### >>> Subjoin 109-0
```
db.region.aggregate(EJSON.deserialize(
[
{"$match":{"$or":[{"r_name":{"$eq":"AFRICA"}},{"r_regionkey":{"$eq":0}}]}}]
));
```
Subjoin plan:
```
COLLSCAN: plan_stability_subjoin_cardinality_md.region {"$or":[{"r_name":{"$eq":"AFRICA"}},{"r_regionkey":{"$eq":0}}]}
```
Estimated cardinality: 1  
Actual cardinality: 1  
Orders of magnitude: 0

---
### >>> Subjoin 109-1
```
db.region.aggregate(EJSON.deserialize(
[
{"$match":{"$or":[{"r_name":{"$eq":"AFRICA"}},{"r_regionkey":{"$eq":0}}]}},
{"$lookup":{"from":"nation","localField":"r_regionkey","foreignField":"n_regionkey","as":"nation","pipeline":[
{"$match":{"$nor":[{"n_regionkey":{"$eq":3}},{"n_name":{"$not":{"$in":["ALGERIA","UNITED STATES"]}}}]}}]}},
{"$unwind":"$nation"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$nation"]}}}]
));
```
Subjoin plan:
```
NLJ r_regionkey = n_regionkey
  -> [region_s] COLLSCAN: plan_stability_subjoin_cardinality_md.region {"$or":[{"r_name":{"$eq":"AFRICA"}},{"r_regionkey":{"$eq":0}}]} 
  -> [nation_s] COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"$nor":[{"n_regionkey":{"$eq":3}},{"n_name":{"$not":{"$in":["ALGERIA","UNITED STATES"]}}}]}
```
Estimated cardinality: 0  
Actual cardinality: 1  
Orders of magnitude: 0

---
### >>> Subjoin 109-2
```
db.region.aggregate(EJSON.deserialize(
[
{"$match":{"$or":[{"r_name":{"$eq":"AFRICA"}},{"r_regionkey":{"$eq":0}}]}},
{"$lookup":{"from":"nation","localField":"r_regionkey","foreignField":"n_regionkey","as":"nation","pipeline":[
{"$match":{"$nor":[{"n_regionkey":{"$eq":3}},{"n_name":{"$not":{"$in":["ALGERIA","UNITED STATES"]}}}]}}]}},
{"$unwind":"$nation"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$nation"]}}},
{"$lookup":{"from":"supplier","localField":"n_nationkey","foreignField":"s_nationkey","as":"supplier","pipeline":[
{"$match":{"$and":[{"$nor":[{"s_name":{"$eq":"Supplier#000000832"}},{"s_nationkey":{"$eq":19}},{"s_acctbal":{"$gte":6835.16}}]},{}]}}]}},
{"$unwind":"$supplier"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$supplier"]}}}]
));
```
Subjoin plan:
```
INLJ nation_s.n_nationkey = s_nationkey
  -> [none] NLJ r_regionkey = n_regionkey
      -> [region_s] COLLSCAN: plan_stability_subjoin_cardinality_md.region {"$or":[{"r_name":{"$eq":"AFRICA"}},{"r_regionkey":{"$eq":0}}]} 
      -> [nation_s] COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"$nor":[{"n_regionkey":{"$eq":3}},{"n_name":{"$not":{"$in":["ALGERIA","UNITED STATES"]}}}]} 
  -> [supplier] FETCH: plan_stability_subjoin_cardinality_md.supplier {"$nor":[{"s_name":{"$eq":"Supplier#000000832"}},{"s_nationkey":{"$eq":19}},{"s_acctbal":{"$gte":6835.16}}]} 
      -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.supplier s_nationkey_1
```
Estimated cardinality: 11  
Actual cardinality: 23  
Orders of magnitude: 0

---
### >>> Subjoin 109-3
```
db.region.aggregate(EJSON.deserialize(
[
{"$match":{"$or":[{"r_name":{"$eq":"AFRICA"}},{"r_regionkey":{"$eq":0}}]}},
{"$lookup":{"from":"nation","localField":"r_regionkey","foreignField":"n_regionkey","as":"nation","pipeline":[
{"$match":{"$nor":[{"n_regionkey":{"$eq":3}},{"n_name":{"$not":{"$in":["ALGERIA","UNITED STATES"]}}}]}}]}},
{"$unwind":"$nation"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$nation"]}}},
{"$lookup":{"from":"supplier","localField":"n_nationkey","foreignField":"s_nationkey","as":"supplier","pipeline":[
{"$match":{"$and":[{"$nor":[{"s_name":{"$eq":"Supplier#000000832"}},{"s_nationkey":{"$eq":19}},{"s_acctbal":{"$gte":6835.16}}]},{}]}}]}},
{"$unwind":"$supplier"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$supplier"]}}},
{"$lookup":{"from":"partsupp","localField":"s_suppkey","foreignField":"ps_suppkey","as":"partsupp","pipeline":[
{"$match":{"$and":[{"ps_comment":{"$regex":"^slyl"}},{}]}}]}},
{"$unwind":"$partsupp"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$partsupp"]}}}]
));
```
Subjoin plan:
```
INLJ supplier.s_suppkey = ps_suppkey
  -> [none] INLJ nation_s.n_nationkey = s_nationkey
      -> [none] NLJ r_regionkey = n_regionkey
          -> [region_s] COLLSCAN: plan_stability_subjoin_cardinality_md.region {"$or":[{"r_name":{"$eq":"AFRICA"}},{"r_regionkey":{"$eq":0}}]} 
          -> [nation_s] COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"$nor":[{"n_regionkey":{"$eq":3}},{"n_name":{"$not":{"$in":["ALGERIA","UNITED STATES"]}}}]} 
      -> [supplier] FETCH: plan_stability_subjoin_cardinality_md.supplier {"$nor":[{"s_name":{"$eq":"Supplier#000000832"}},{"s_nationkey":{"$eq":19}},{"s_acctbal":{"$gte":6835.16}}]} 
          -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.supplier s_nationkey_1
  -> [none] FETCH: plan_stability_subjoin_cardinality_md.partsupp {"ps_comment":{"$regex":"^slyl"}} 
      -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.partsupp ps_suppkey_1
```
Estimated cardinality: 7  
Actual cardinality: 13  
Orders of magnitude: 1

---
## >>> Command idx 110
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$or":[{"ps_supplycost":{"$lt":72.58}}]}},
{"$match":{"$nor":[{"ps_availqty":{"$lt":9806}},{"ps_comment":{"$regex":{"$regex":"^i","$options":""}}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"lineitem","localField":"partsupp.ps_partkey","foreignField":"l_partkey","pipeline":[
{"$match":{"$nor":[{"l_shipdate":{"$lte":"1992-11-11T00:00:00.000Z"}},{"l_suppkey":237},{"l_suppkey":40},{"l_linenumber":{"$lte":3}},{"l_orderkey":342531}]}}],"as":"lineitem"}},
{"$unwind":"$lineitem"},
{"$match":{"$nor":[{"lineitem.l_linenumber":{"$gt":6}}]}}],"cursor":{},"idx":110}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 111
```
{"aggregate":"partsupp","pipeline":[
{"$lookup":{"from":"supplier","localField":"ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$and":[{"s_acctbal":{"$lte":10.33}}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$and":[{"n_regionkey":0}]}},
{"$match":{"$nor":[{"n_name":"RUSSIA"}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$lookup":{"from":"region","localField":"nation_s.n_regionkey","foreignField":"r_regionkey","pipeline":[
{"$match":{"$or":[{"r_regionkey":0},{"r_regionkey":4},{"r_name":"AFRICA"}]}}],"as":"region_s"}},
{"$unwind":"$region_s"},
{"$match":{"$or":[{"ps_availqty":{"$lt":5779}}]}}],"cursor":{},"idx":111}
```
### >>> Subjoin 111-0
```
db.region.aggregate(EJSON.deserialize(
[
{"$match":{"$or":[{"r_name":{"$eq":"AFRICA"}},{"r_regionkey":{"$in":[0,4]}}]}}]
));
```
Subjoin plan:
```
COLLSCAN: plan_stability_subjoin_cardinality_md.region {"$or":[{"r_name":{"$eq":"AFRICA"}},{"r_regionkey":{"$in":[0,4]}}]}
```
Estimated cardinality: 2  
Actual cardinality: 2  
Orders of magnitude: 0

---
### >>> Subjoin 111-1
```
db.region.aggregate(EJSON.deserialize(
[
{"$match":{"$or":[{"r_name":{"$eq":"AFRICA"}},{"r_regionkey":{"$in":[0,4]}}]}},
{"$lookup":{"from":"nation","localField":"r_regionkey","foreignField":"n_regionkey","as":"nation","pipeline":[
{"$match":{"$and":[{"$and":[{"n_regionkey":{"$eq":0}},{"n_name":{"$not":{"$eq":"RUSSIA"}}}]},{}]}}]}},
{"$unwind":"$nation"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$nation"]}}}]
));
```
Subjoin plan:
```
INLJ r_regionkey = n_regionkey
  -> [region_s] COLLSCAN: plan_stability_subjoin_cardinality_md.region {"$or":[{"r_name":{"$eq":"AFRICA"}},{"r_regionkey":{"$in":[0,4]}}]} 
  -> [nation_s] FETCH: plan_stability_subjoin_cardinality_md.nation {"$and":[{"n_regionkey":{"$eq":0}},{"n_name":{"$not":{"$eq":"RUSSIA"}}}]} 
      -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.nation n_regionkey_1
```
Estimated cardinality: 2  
Actual cardinality: 5  
Orders of magnitude: 0

---
### >>> Subjoin 111-2
```
db.region.aggregate(EJSON.deserialize(
[
{"$match":{"$or":[{"r_name":{"$eq":"AFRICA"}},{"r_regionkey":{"$in":[0,4]}}]}},
{"$lookup":{"from":"nation","localField":"r_regionkey","foreignField":"n_regionkey","as":"nation","pipeline":[
{"$match":{"$and":[{"$and":[{"n_regionkey":{"$eq":0}},{"n_name":{"$not":{"$eq":"RUSSIA"}}}]},{}]}}]}},
{"$unwind":"$nation"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$nation"]}}},
{"$lookup":{"from":"supplier","localField":"n_nationkey","foreignField":"s_nationkey","as":"supplier","pipeline":[
{"$match":{"$and":[{"s_acctbal":{"$lte":10.33}},{}]}}]}},
{"$unwind":"$supplier"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$supplier"]}}}]
));
```
Subjoin plan:
```
INLJ nation_s.n_nationkey = s_nationkey
  -> [none] INLJ r_regionkey = n_regionkey
      -> [region_s] COLLSCAN: plan_stability_subjoin_cardinality_md.region {"$or":[{"r_name":{"$eq":"AFRICA"}},{"r_regionkey":{"$in":[0,4]}}]} 
      -> [nation_s] FETCH: plan_stability_subjoin_cardinality_md.nation {"$and":[{"n_regionkey":{"$eq":0}},{"n_name":{"$not":{"$eq":"RUSSIA"}}}]} 
          -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.nation n_regionkey_1
  -> [supplier] FETCH: plan_stability_subjoin_cardinality_md.supplier {"s_acctbal":{"$lte":10.33}} 
      -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.supplier s_nationkey_1
```
Estimated cardinality: 7  
Actual cardinality: 13  
Orders of magnitude: 1

---
### >>> Subjoin 111-3
```
db.region.aggregate(EJSON.deserialize(
[
{"$match":{"$or":[{"r_name":{"$eq":"AFRICA"}},{"r_regionkey":{"$in":[0,4]}}]}},
{"$lookup":{"from":"nation","localField":"r_regionkey","foreignField":"n_regionkey","as":"nation","pipeline":[
{"$match":{"$and":[{"$and":[{"n_regionkey":{"$eq":0}},{"n_name":{"$not":{"$eq":"RUSSIA"}}}]},{}]}}]}},
{"$unwind":"$nation"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$nation"]}}},
{"$lookup":{"from":"supplier","localField":"n_nationkey","foreignField":"s_nationkey","as":"supplier","pipeline":[
{"$match":{"$and":[{"s_acctbal":{"$lte":10.33}},{}]}}]}},
{"$unwind":"$supplier"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$supplier"]}}},
{"$lookup":{"from":"partsupp","localField":"s_suppkey","foreignField":"ps_suppkey","as":"partsupp","pipeline":[
{"$match":{"$and":[{"ps_availqty":{"$lt":5779}},{}]}}]}},
{"$unwind":"$partsupp"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$partsupp"]}}}]
));
```
Subjoin plan:
```
INLJ supplier.s_suppkey = ps_suppkey
  -> [none] INLJ nation_s.n_nationkey = s_nationkey
      -> [none] INLJ r_regionkey = n_regionkey
          -> [region_s] COLLSCAN: plan_stability_subjoin_cardinality_md.region {"$or":[{"r_name":{"$eq":"AFRICA"}},{"r_regionkey":{"$in":[0,4]}}]} 
          -> [nation_s] FETCH: plan_stability_subjoin_cardinality_md.nation {"$and":[{"n_regionkey":{"$eq":0}},{"n_name":{"$not":{"$eq":"RUSSIA"}}}]} 
              -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.nation n_regionkey_1
      -> [supplier] FETCH: plan_stability_subjoin_cardinality_md.supplier {"s_acctbal":{"$lte":10.33}} 
          -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.supplier s_nationkey_1
  -> [none] FETCH: plan_stability_subjoin_cardinality_md.partsupp {"ps_availqty":{"$lt":5779}} 
      -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.partsupp ps_suppkey_1
```
Estimated cardinality: 335  
Actual cardinality: 566  
Orders of magnitude: 0

---
## >>> Command idx 112
```
{"aggregate":"supplier","pipeline":[
{"$lookup":{"from":"nation","localField":"s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$nor":[{"n_regionkey":4},{"n_regionkey":0}]}},
{"$match":{"$or":[{"n_name":{"$nin":["INDONESIA","ETHIOPIA"]}}]}},
{"$match":{"$nor":[{"n_name":{"$nin":["UNITED STATES","RUSSIA","JAPAN","CANADA","EGYPT"]}},{"n_regionkey":2}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$lookup":{"from":"region","localField":"nation_s.n_regionkey","foreignField":"r_regionkey","pipeline":[
{"$match":{"$nor":[{"r_regionkey":3}]}},
{"$match":{"$nor":[{"r_name":"MIDDLE EAST"}]}}],"as":"region_s"}},
{"$unwind":"$region_s"},
{"$match":{"$or":[{"region_s.r_regionkey":3},{"region_s.r_name":"EUROPE"},{"nation_s.n_name":"RUSSIA"},{"s_acctbal":{"$lt":6404.51}}]}}],"cursor":{},"idx":112}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 113
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$nor":[{"ps_comment":{"$regex":{"$regex":"^ve","$options":""}}}]}},
{"$match":{"$and":[{"ps_comment":{"$regex":{"$regex":"^.","$options":""}}}]}},
{"$match":{"$or":[{"ps_comment":{"$regex":{"$regex":"^eque","$options":""}}},{"ps_supplycost":{"$gt":889.05}},{"ps_supplycost":{"$eq":136.6}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"lineitem","localField":"partsupp.ps_partkey","foreignField":"l_partkey","pipeline":[
{"$match":{"$or":[{"l_orderkey":100355},{"l_shipinstruct":"DELIVER IN PERSON"}]}}],"as":"lineitem"}},
{"$unwind":"$lineitem"},
{"$match":{"$nor":[{"lineitem.l_shipinstruct":"DELIVER IN PERSON"},{"p_comment":{"$regex":{"$regex":"^uick","$options":""}}}]}}],"cursor":{},"idx":113}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 114
```
{"aggregate":"lineitem","pipeline":[
{"$lookup":{"from":"orders","localField":"l_orderkey","foreignField":"o_orderkey","pipeline":[
{"$match":{"$and":[{"o_totalprice":{"$lte":116740.24}},{"o_shippriority":{"$lte":0}}]}},
{"$match":{"$or":[{"o_orderdate":{"$lte":"1995-02-03T00:00:00.000Z"}}]}}],"as":"orders"}},
{"$unwind":"$orders"},
{"$lookup":{"from":"customer","localField":"orders.o_custkey","foreignField":"c_custkey","pipeline":[
{"$match":{"$or":[{"c_mktsegment":"AUTOMOBILE"},{"c_mktsegment":"AUTOMOBILE"},{"c_mktsegment":"FURNITURE"},{"c_name":"Customer#000013077"},{"c_name":"Customer#000002309"}]}}],"as":"customer"}},
{"$unwind":"$customer"},
{"$lookup":{"from":"supplier","localField":"customer.c_nationkey","foreignField":"s_nationkey","pipeline":[
{"$match":{"$nor":[{"s_name":"Supplier#000000119"}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$match":{"$and":[{"l_returnflag":{"$in":["N","R"]}},{"customer.c_name":"Customer#000010639"}]}}],"cursor":{},"idx":114}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 115
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$nor":[{"ps_availqty":{"$lt":2302}}]}},
{"$match":{"$nor":[{"ps_supplycost":{"$lt":175.44}},{"ps_supplycost":{"$lte":892.51}},{"ps_comment":{"$regex":{"$regex":"^ordi","$options":""}}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"supplier","localField":"partsupp.ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$nor":[{"s_acctbal":{"$lte":9524.84}}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$nor":[{"n_name":"ROMANIA"},{"n_name":"INDIA"}]}},
{"$match":{"$or":[{"n_regionkey":3}]}},
{"$match":{"$nor":[{"n_regionkey":0}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$match":{"$and":[{"nation_s.n_regionkey":3}]}}],"cursor":{},"idx":115}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 116
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$nor":[{"ps_comment":{"$regex":{"$regex":"^quic","$options":""}}},{"ps_comment":{"$regex":{"$regex":"^l","$options":""}}},{"ps_availqty":{"$lte":8791}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"supplier","localField":"partsupp.ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$nor":[{"s_acctbal":{"$gt":3222.71}}]}},
{"$match":{"$or":[{"s_acctbal":{"$gt":-467.16}},{"s_acctbal":{"$gte":8924.02}}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$or":[{"n_regionkey":4}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$lookup":{"from":"region","localField":"nation_s.n_regionkey","foreignField":"r_regionkey","pipeline":[
{"$match":{"$nor":[{"r_name":"AFRICA"}]}}],"as":"region_s"}},
{"$unwind":"$region_s"},
{"$match":{"$or":[{"p_brand":{"$nin":["Brand#52","Brand#32","Brand#54"]}},{"region_s.r_name":"AMERICA"}]}}],"cursor":{},"idx":116}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 117
```
{"aggregate":"supplier","pipeline":[
{"$lookup":{"from":"nation","localField":"s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$nor":[{"n_regionkey":0},{"n_name":{"$in":["JAPAN","JAPAN","GERMANY","ETHIOPIA","INDONESIA"]}}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$lookup":{"from":"region","localField":"nation_s.n_regionkey","foreignField":"r_regionkey","pipeline":[
{"$match":{"$and":[{"r_name":"ASIA"},{"r_regionkey":2}]}}],"as":"region_s"}},
{"$unwind":"$region_s"},
{"$match":{"$nor":[{"s_acctbal":{"$lte":7619.85}},{"region_s.r_regionkey":3}]}}],"cursor":{},"idx":117}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 118
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$nor":[{"ps_availqty":{"$eq":6013}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"supplier","localField":"partsupp.ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$nor":[{"s_name":"Supplier#000000319"},{"s_nationkey":20},{"s_nationkey":21},{"s_nationkey":21}]}},
{"$match":{"$or":[{"s_nationkey":20},{"s_acctbal":{"$lt":-297.76}}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$nor":[{"n_name":"VIETNAM"}]}},
{"$match":{"$and":[{"n_name":"ARGENTINA"}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$match":{"$and":[{"p_brand":{"$nin":["Brand#44","Brand#24","Brand#31"]}}]}}],"cursor":{},"idx":118}
```
### >>> Subjoin 118-0
```
db.nation.aggregate(EJSON.deserialize(
[
{"$match":{"$and":[{"n_name":{"$eq":"ARGENTINA"}},{"n_name":{"$not":{"$eq":"VIETNAM"}}}]}}]
));
```
Subjoin plan:
```
COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"$and":[{"n_name":{"$eq":"ARGENTINA"}},{"n_name":{"$not":{"$eq":"VIETNAM"}}}]}
```
Estimated cardinality: 1  
Actual cardinality: 1  
Orders of magnitude: 0

---
### >>> Subjoin 118-1
```
db.nation.aggregate(EJSON.deserialize(
[
{"$match":{"$and":[{"n_name":{"$eq":"ARGENTINA"}},{"n_name":{"$not":{"$eq":"VIETNAM"}}}]}},
{"$lookup":{"from":"supplier","localField":"n_nationkey","foreignField":"s_nationkey","as":"supplier","pipeline":[
{"$match":{"$and":[{"$and":[{"$or":[{"s_nationkey":{"$eq":20}},{"s_acctbal":{"$lt":-297.76}}]},{"$nor":[{"s_name":{"$eq":"Supplier#000000319"}},{"s_nationkey":{"$eq":20}},{"s_nationkey":{"$eq":21}},{"s_nationkey":{"$eq":21}}]}]},{}]}}]}},
{"$unwind":"$supplier"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$supplier"]}}}]
));
```
Subjoin plan:
```
INLJ n_nationkey = s_nationkey
  -> [nation_s] COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"$and":[{"n_name":{"$eq":"ARGENTINA"}},{"n_name":{"$not":{"$eq":"VIETNAM"}}}]} 
  -> [supplier] FETCH: plan_stability_subjoin_cardinality_md.supplier {"$and":[{"$or":[{"s_nationkey":{"$eq":20}},{"s_acctbal":{"$lt":-297.76}}]},{"$nor":[{"s_name":{"$eq":"Supplier#000000319"}},{"s_nationkey":{"$eq":20}},{"s_nationkey":{"$eq":21}},{"s_nationkey":{"$eq":21}}]}]} 
      -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.supplier s_nationkey_1
```
Estimated cardinality: 2  
Actual cardinality: 2  
Orders of magnitude: 0

---
### >>> Subjoin 118-2
```
db.nation.aggregate(EJSON.deserialize(
[
{"$match":{"$and":[{"n_name":{"$eq":"ARGENTINA"}},{"n_name":{"$not":{"$eq":"VIETNAM"}}}]}},
{"$lookup":{"from":"supplier","localField":"n_nationkey","foreignField":"s_nationkey","as":"supplier","pipeline":[
{"$match":{"$and":[{"$and":[{"$or":[{"s_nationkey":{"$eq":20}},{"s_acctbal":{"$lt":-297.76}}]},{"$nor":[{"s_name":{"$eq":"Supplier#000000319"}},{"s_nationkey":{"$eq":20}},{"s_nationkey":{"$eq":21}},{"s_nationkey":{"$eq":21}}]}]},{}]}}]}},
{"$unwind":"$supplier"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$supplier"]}}},
{"$lookup":{"from":"partsupp","localField":"s_suppkey","foreignField":"ps_suppkey","as":"partsupp","pipeline":[
{"$match":{"$and":[{"ps_availqty":{"$not":{"$eq":6013}}},{}]}}]}},
{"$unwind":"$partsupp"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$partsupp"]}}}]
));
```
Subjoin plan:
```
INLJ supplier.s_suppkey = ps_suppkey
  -> [none] INLJ n_nationkey = s_nationkey
      -> [nation_s] COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"$and":[{"n_name":{"$eq":"ARGENTINA"}},{"n_name":{"$not":{"$eq":"VIETNAM"}}}]} 
      -> [supplier] FETCH: plan_stability_subjoin_cardinality_md.supplier {"$and":[{"$or":[{"s_nationkey":{"$eq":20}},{"s_acctbal":{"$lt":-297.76}}]},{"$nor":[{"s_name":{"$eq":"Supplier#000000319"}},{"s_nationkey":{"$eq":20}},{"s_nationkey":{"$eq":21}},{"s_nationkey":{"$eq":21}}]}]} 
          -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.supplier s_nationkey_1
  -> [partsupp] FETCH: plan_stability_subjoin_cardinality_md.partsupp {"ps_availqty":{"$not":{"$eq":6013}}} 
      -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.partsupp ps_suppkey_1
```
Estimated cardinality: 182  
Actual cardinality: 160  
Orders of magnitude: 0

---
### >>> Subjoin 118-3
```
db.nation.aggregate(EJSON.deserialize(
[
{"$match":{"$and":[{"n_name":{"$eq":"ARGENTINA"}},{"n_name":{"$not":{"$eq":"VIETNAM"}}}]}},
{"$lookup":{"from":"supplier","localField":"n_nationkey","foreignField":"s_nationkey","as":"supplier","pipeline":[
{"$match":{"$and":[{"$and":[{"$or":[{"s_nationkey":{"$eq":20}},{"s_acctbal":{"$lt":-297.76}}]},{"$nor":[{"s_name":{"$eq":"Supplier#000000319"}},{"s_nationkey":{"$eq":20}},{"s_nationkey":{"$eq":21}},{"s_nationkey":{"$eq":21}}]}]},{}]}}]}},
{"$unwind":"$supplier"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$supplier"]}}},
{"$lookup":{"from":"partsupp","localField":"s_suppkey","foreignField":"ps_suppkey","as":"partsupp","pipeline":[
{"$match":{"$and":[{"ps_availqty":{"$not":{"$eq":6013}}},{}]}}]}},
{"$unwind":"$partsupp"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$partsupp"]}}},
{"$lookup":{"from":"part","localField":"ps_partkey","foreignField":"p_partkey","as":"part","pipeline":[
{"$match":{"p_brand":{"$not":{"$in":["Brand#24","Brand#31","Brand#44"]}}}}]}},
{"$unwind":"$part"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$part"]}}}]
));
```
Subjoin plan:
```
HJ partsupp.ps_partkey = p_partkey
  -> [none] INLJ supplier.s_suppkey = ps_suppkey
      -> [none] INLJ n_nationkey = s_nationkey
          -> [nation_s] COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"$and":[{"n_name":{"$eq":"ARGENTINA"}},{"n_name":{"$not":{"$eq":"VIETNAM"}}}]} 
          -> [supplier] FETCH: plan_stability_subjoin_cardinality_md.supplier {"$and":[{"$or":[{"s_nationkey":{"$eq":20}},{"s_acctbal":{"$lt":-297.76}}]},{"$nor":[{"s_name":{"$eq":"Supplier#000000319"}},{"s_nationkey":{"$eq":20}},{"s_nationkey":{"$eq":21}},{"s_nationkey":{"$eq":21}}]}]} 
              -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.supplier s_nationkey_1
      -> [partsupp] FETCH: plan_stability_subjoin_cardinality_md.partsupp {"ps_availqty":{"$not":{"$eq":6013}}} 
          -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.partsupp ps_suppkey_1
  -> [none] COLLSCAN: plan_stability_subjoin_cardinality_md.part {"p_brand":{"$not":{"$in":["Brand#24","Brand#31","Brand#44"]}}}
```
Estimated cardinality: 161  
Actual cardinality: 139  
Orders of magnitude: 0

---
## >>> Command idx 119
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$or":[{"ps_comment":{"$regex":{"$regex":"^blit","$options":""}}}]}},
{"$match":{"$nor":[{"ps_availqty":{"$eq":3385}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"supplier","localField":"partsupp.ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$nor":[{"s_nationkey":20}]}},
{"$match":{"$or":[{"s_acctbal":{"$gte":2781.03}},{"s_acctbal":{"$gte":7182.24}}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$or":[{"n_name":{"$nin":["UNITED STATES","ROMANIA"]}},{"n_regionkey":2}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$lookup":{"from":"region","localField":"nation_s.n_regionkey","foreignField":"r_regionkey","pipeline":[
{"$match":{"$nor":[{"r_regionkey":0},{"r_regionkey":3},{"r_regionkey":1}]}}],"as":"region_s"}},
{"$unwind":"$region_s"},
{"$match":{"$nor":[{"p_partkey":12630}]}}],"cursor":{},"idx":119}
```
### >>> Subjoin 119-0
```
db.region.aggregate(EJSON.deserialize(
[
{"$match":{"$nor":[{"r_regionkey":{"$eq":0}},{"r_regionkey":{"$eq":3}},{"r_regionkey":{"$eq":1}}]}}]
));
```
Subjoin plan:
```
COLLSCAN: plan_stability_subjoin_cardinality_md.region {"$nor":[{"r_regionkey":{"$eq":0}},{"r_regionkey":{"$eq":3}},{"r_regionkey":{"$eq":1}}]}
```
Estimated cardinality: 2  
Actual cardinality: 2  
Orders of magnitude: 0

---
### >>> Subjoin 119-1
```
db.region.aggregate(EJSON.deserialize(
[
{"$match":{"$nor":[{"r_regionkey":{"$eq":0}},{"r_regionkey":{"$eq":3}},{"r_regionkey":{"$eq":1}}]}},
{"$lookup":{"from":"nation","localField":"r_regionkey","foreignField":"n_regionkey","as":"nation","pipeline":[
{"$match":{"$or":[{"n_regionkey":{"$eq":2}},{"n_name":{"$not":{"$in":["ROMANIA","UNITED STATES"]}}}]}}]}},
{"$unwind":"$nation"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$nation"]}}}]
));
```
Subjoin plan:
```
HJ r_regionkey = n_regionkey
  -> [region_s] COLLSCAN: plan_stability_subjoin_cardinality_md.region {"$nor":[{"r_regionkey":{"$eq":0}},{"r_regionkey":{"$eq":3}},{"r_regionkey":{"$eq":1}}]} 
  -> [nation_s] COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"$or":[{"n_regionkey":{"$eq":2}},{"n_name":{"$not":{"$in":["ROMANIA","UNITED STATES"]}}}]}
```
Estimated cardinality: 9  
Actual cardinality: 10  
Orders of magnitude: 1

---
### >>> Subjoin 119-2
```
db.region.aggregate(EJSON.deserialize(
[
{"$match":{"$nor":[{"r_regionkey":{"$eq":0}},{"r_regionkey":{"$eq":3}},{"r_regionkey":{"$eq":1}}]}},
{"$lookup":{"from":"nation","localField":"r_regionkey","foreignField":"n_regionkey","as":"nation","pipeline":[
{"$match":{"$or":[{"n_regionkey":{"$eq":2}},{"n_name":{"$not":{"$in":["ROMANIA","UNITED STATES"]}}}]}}]}},
{"$unwind":"$nation"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$nation"]}}},
{"$lookup":{"from":"supplier","localField":"n_nationkey","foreignField":"s_nationkey","as":"supplier","pipeline":[
{"$match":{"$and":[{"$or":[{"s_acctbal":{"$gte":2781.03}},{"s_acctbal":{"$gte":7182.24}}]},{"s_nationkey":{"$not":{"$eq":20}}}]}}]}},
{"$unwind":"$supplier"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$supplier"]}}}]
));
```
Subjoin plan:
```
HJ nation_s.n_nationkey = s_nationkey
  -> [none] HJ r_regionkey = n_regionkey
      -> [region_s] COLLSCAN: plan_stability_subjoin_cardinality_md.region {"$nor":[{"r_regionkey":{"$eq":0}},{"r_regionkey":{"$eq":3}},{"r_regionkey":{"$eq":1}}]} 
      -> [nation_s] COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"$or":[{"n_regionkey":{"$eq":2}},{"n_name":{"$not":{"$in":["ROMANIA","UNITED STATES"]}}}]} 
  -> [supplier] COLLSCAN: plan_stability_subjoin_cardinality_md.supplier {"$and":[{"$or":[{"s_acctbal":{"$gte":2781.03}},{"s_acctbal":{"$gte":7182.24}}]},{"s_nationkey":{"$not":{"$eq":20}}}]}
```
Estimated cardinality: 227  
Actual cardinality: 237  
Orders of magnitude: 0

---
### >>> Subjoin 119-3
```
db.region.aggregate(EJSON.deserialize(
[
{"$match":{"$nor":[{"r_regionkey":{"$eq":0}},{"r_regionkey":{"$eq":3}},{"r_regionkey":{"$eq":1}}]}},
{"$lookup":{"from":"nation","localField":"r_regionkey","foreignField":"n_regionkey","as":"nation","pipeline":[
{"$match":{"$or":[{"n_regionkey":{"$eq":2}},{"n_name":{"$not":{"$in":["ROMANIA","UNITED STATES"]}}}]}}]}},
{"$unwind":"$nation"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$nation"]}}},
{"$lookup":{"from":"supplier","localField":"n_nationkey","foreignField":"s_nationkey","as":"supplier","pipeline":[
{"$match":{"$and":[{"$or":[{"s_acctbal":{"$gte":2781.03}},{"s_acctbal":{"$gte":7182.24}}]},{"s_nationkey":{"$not":{"$eq":20}}}]}}]}},
{"$unwind":"$supplier"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$supplier"]}}},
{"$lookup":{"from":"partsupp","localField":"s_suppkey","foreignField":"ps_suppkey","as":"partsupp","pipeline":[
{"$match":{"$and":[{"ps_comment":{"$regex":"^blit"}},{"ps_availqty":{"$not":{"$eq":3385}}}]}}]}},
{"$unwind":"$partsupp"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$partsupp"]}}}]
));
```
Subjoin plan:
```
HJ supplier.s_suppkey = ps_suppkey
  -> [none] HJ nation_s.n_nationkey = s_nationkey
      -> [none] HJ r_regionkey = n_regionkey
          -> [region_s] COLLSCAN: plan_stability_subjoin_cardinality_md.region {"$nor":[{"r_regionkey":{"$eq":0}},{"r_regionkey":{"$eq":3}},{"r_regionkey":{"$eq":1}}]} 
          -> [nation_s] COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"$or":[{"n_regionkey":{"$eq":2}},{"n_name":{"$not":{"$in":["ROMANIA","UNITED STATES"]}}}]} 
      -> [supplier] COLLSCAN: plan_stability_subjoin_cardinality_md.supplier {"$and":[{"$or":[{"s_acctbal":{"$gte":2781.03}},{"s_acctbal":{"$gte":7182.24}}]},{"s_nationkey":{"$not":{"$eq":20}}}]} 
  -> [partsupp] COLLSCAN: plan_stability_subjoin_cardinality_md.partsupp {"$and":[{"ps_comment":{"$regex":"^blit"}},{"ps_availqty":{"$not":{"$eq":3385}}}]}
```
Estimated cardinality: 91  
Actual cardinality: 80  
Orders of magnitude: 0

---
### >>> Subjoin 119-4
```
db.region.aggregate(EJSON.deserialize(
[
{"$match":{"$nor":[{"r_regionkey":{"$eq":0}},{"r_regionkey":{"$eq":3}},{"r_regionkey":{"$eq":1}}]}},
{"$lookup":{"from":"nation","localField":"r_regionkey","foreignField":"n_regionkey","as":"nation","pipeline":[
{"$match":{"$or":[{"n_regionkey":{"$eq":2}},{"n_name":{"$not":{"$in":["ROMANIA","UNITED STATES"]}}}]}}]}},
{"$unwind":"$nation"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$nation"]}}},
{"$lookup":{"from":"supplier","localField":"n_nationkey","foreignField":"s_nationkey","as":"supplier","pipeline":[
{"$match":{"$and":[{"$or":[{"s_acctbal":{"$gte":2781.03}},{"s_acctbal":{"$gte":7182.24}}]},{"s_nationkey":{"$not":{"$eq":20}}}]}}]}},
{"$unwind":"$supplier"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$supplier"]}}},
{"$lookup":{"from":"partsupp","localField":"s_suppkey","foreignField":"ps_suppkey","as":"partsupp","pipeline":[
{"$match":{"$and":[{"ps_comment":{"$regex":"^blit"}},{"ps_availqty":{"$not":{"$eq":3385}}}]}}]}},
{"$unwind":"$partsupp"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$partsupp"]}}},
{"$lookup":{"from":"part","localField":"ps_partkey","foreignField":"p_partkey","as":"part","pipeline":[
{"$match":{"$and":[{"p_partkey":{"$not":{"$eq":12630}}},{}]}}]}},
{"$unwind":"$part"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$part"]}}}]
));
```
Subjoin plan:
```
INLJ partsupp.ps_partkey = p_partkey
  -> [none] HJ supplier.s_suppkey = ps_suppkey
      -> [none] HJ nation_s.n_nationkey = s_nationkey
          -> [none] HJ r_regionkey = n_regionkey
              -> [region_s] COLLSCAN: plan_stability_subjoin_cardinality_md.region {"$nor":[{"r_regionkey":{"$eq":0}},{"r_regionkey":{"$eq":3}},{"r_regionkey":{"$eq":1}}]} 
              -> [nation_s] COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"$or":[{"n_regionkey":{"$eq":2}},{"n_name":{"$not":{"$in":["ROMANIA","UNITED STATES"]}}}]} 
          -> [supplier] COLLSCAN: plan_stability_subjoin_cardinality_md.supplier {"$and":[{"$or":[{"s_acctbal":{"$gte":2781.03}},{"s_acctbal":{"$gte":7182.24}}]},{"s_nationkey":{"$not":{"$eq":20}}}]} 
      -> [partsupp] COLLSCAN: plan_stability_subjoin_cardinality_md.partsupp {"$and":[{"ps_comment":{"$regex":"^blit"}},{"ps_availqty":{"$not":{"$eq":3385}}}]} 
  -> [none] FETCH: plan_stability_subjoin_cardinality_md.part {"p_partkey":{"$not":{"$eq":12630}}} 
      -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.part p_partkey_1
```
Estimated cardinality: 91  
Actual cardinality: 80  
Orders of magnitude: 0

---
## >>> Command idx 120
```
{"aggregate":"supplier","pipeline":[
{"$lookup":{"from":"nation","localField":"s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$or":[{"n_name":"INDIA"}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$lookup":{"from":"region","localField":"nation_s.n_regionkey","foreignField":"r_regionkey","pipeline":[
{"$match":{"$and":[{"r_name":"ASIA"}]}}],"as":"region_s"}},
{"$unwind":"$region_s"},
{"$match":{"$nor":[{"s_nationkey":19},{"region_s.r_regionkey":3}]}}],"cursor":{},"idx":120}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 121
```
{"aggregate":"supplier","pipeline":[
{"$lookup":{"from":"nation","localField":"s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$or":[{"n_regionkey":4},{"n_name":"UNITED KINGDOM"},{"n_name":{"$nin":["VIETNAM","RUSSIA","VIETNAM","JAPAN"]}}]}},
{"$match":{"$and":[{"n_name":{"$in":["ALGERIA","JORDAN"]}}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$lookup":{"from":"region","localField":"nation_s.n_regionkey","foreignField":"r_regionkey","pipeline":[
{"$match":{"$nor":[{"r_regionkey":3},{"r_regionkey":3},{"r_name":"AFRICA"}]}}],"as":"region_s"}},
{"$unwind":"$region_s"},
{"$match":{"$or":[{"s_acctbal":{"$gt":4680.75}}]}}],"cursor":{},"idx":121}
```
### >>> Subjoin 121-0
```
db.nation.aggregate(EJSON.deserialize(
[
{"$match":{"$and":[{"$or":[{"n_name":{"$eq":"UNITED KINGDOM"}},{"n_regionkey":{"$eq":4}},{"n_name":{"$not":{"$in":["JAPAN","RUSSIA","VIETNAM"]}}}]},{"n_name":{"$in":["ALGERIA","JORDAN"]}}]}}]
));
```
Subjoin plan:
```
COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"$and":[{"$or":[{"n_name":{"$eq":"UNITED KINGDOM"}},{"n_regionkey":{"$eq":4}},{"n_name":{"$not":{"$in":["JAPAN","RUSSIA","VIETNAM"]}}}]},{"n_name":{"$in":["ALGERIA","JORDAN"]}}]}
```
Estimated cardinality: 2  
Actual cardinality: 2  
Orders of magnitude: 0

---
### >>> Subjoin 121-1
```
db.nation.aggregate(EJSON.deserialize(
[
{"$match":{"$and":[{"$or":[{"n_name":{"$eq":"UNITED KINGDOM"}},{"n_regionkey":{"$eq":4}},{"n_name":{"$not":{"$in":["JAPAN","RUSSIA","VIETNAM"]}}}]},{"n_name":{"$in":["ALGERIA","JORDAN"]}}]}},
{"$lookup":{"from":"region","localField":"n_regionkey","foreignField":"r_regionkey","as":"region","pipeline":[
{"$match":{"$nor":[{"r_name":{"$eq":"AFRICA"}},{"r_regionkey":{"$eq":3}},{"r_regionkey":{"$eq":3}}]}}]}},
{"$unwind":"$region"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$region"]}}}]
));
```
Subjoin plan:
```
HJ n_regionkey = r_regionkey
  -> [nation_s] COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"$and":[{"$or":[{"n_name":{"$eq":"UNITED KINGDOM"}},{"n_regionkey":{"$eq":4}},{"n_name":{"$not":{"$in":["JAPAN","RUSSIA","VIETNAM"]}}}]},{"n_name":{"$in":["ALGERIA","JORDAN"]}}]} 
  -> [region_s] COLLSCAN: plan_stability_subjoin_cardinality_md.region {"$nor":[{"r_name":{"$eq":"AFRICA"}},{"r_regionkey":{"$eq":3}},{"r_regionkey":{"$eq":3}}]}
```
Estimated cardinality: 1  
Actual cardinality: 1  
Orders of magnitude: 0

---
### >>> Subjoin 121-2
```
db.nation.aggregate(EJSON.deserialize(
[
{"$match":{"$and":[{"$or":[{"n_name":{"$eq":"UNITED KINGDOM"}},{"n_regionkey":{"$eq":4}},{"n_name":{"$not":{"$in":["JAPAN","RUSSIA","VIETNAM"]}}}]},{"n_name":{"$in":["ALGERIA","JORDAN"]}}]}},
{"$lookup":{"from":"region","localField":"n_regionkey","foreignField":"r_regionkey","as":"region","pipeline":[
{"$match":{"$nor":[{"r_name":{"$eq":"AFRICA"}},{"r_regionkey":{"$eq":3}},{"r_regionkey":{"$eq":3}}]}}]}},
{"$unwind":"$region"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$region"]}}},
{"$lookup":{"from":"supplier","localField":"n_nationkey","foreignField":"s_nationkey","as":"supplier","pipeline":[
{"$match":{"$and":[{"s_acctbal":{"$gt":4680.75}},{}]}}]}},
{"$unwind":"$supplier"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$supplier"]}}}]
));
```
Subjoin plan:
```
INLJ nation_s.n_nationkey = s_nationkey
  -> [none] HJ n_regionkey = r_regionkey
      -> [nation_s] COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"$and":[{"$or":[{"n_name":{"$eq":"UNITED KINGDOM"}},{"n_regionkey":{"$eq":4}},{"n_name":{"$not":{"$in":["JAPAN","RUSSIA","VIETNAM"]}}}]},{"n_name":{"$in":["ALGERIA","JORDAN"]}}]} 
      -> [region_s] COLLSCAN: plan_stability_subjoin_cardinality_md.region {"$nor":[{"r_name":{"$eq":"AFRICA"}},{"r_regionkey":{"$eq":3}},{"r_regionkey":{"$eq":3}}]} 
  -> [none] FETCH: plan_stability_subjoin_cardinality_md.supplier {"s_acctbal":{"$gt":4680.75}} 
      -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.supplier s_nationkey_1
```
Estimated cardinality: 23  
Actual cardinality: 13  
Orders of magnitude: 0

---
## >>> Command idx 122
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$and":[{"ps_availqty":{"$lt":7155}},{"ps_comment":{"$regex":{"$regex":"^s. ","$options":""}}}]}},
{"$match":{"$nor":[{"ps_comment":{"$regex":{"$regex":"^ ","$options":""}}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"supplier","localField":"partsupp.ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$nor":[{"s_name":"Supplier#000000587"},{"s_acctbal":{"$lte":6399.78}}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$or":[{"n_name":{"$in":["SAUDI ARABIA","ROMANIA"]}},{"n_regionkey":4},{"n_regionkey":3}]}},
{"$match":{"$and":[{"n_name":{"$nin":["UNITED STATES","VIETNAM","CHINA","ETHIOPIA"]}}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$match":{"$or":[{"p_name":{"$regex":{"$regex":"^co","$options":""}}}]}}],"cursor":{},"idx":122}
```
### >>> Subjoin 122-0
```
db.nation.aggregate(EJSON.deserialize(
[
{"$match":{"$and":[{"$or":[{"n_name":{"$in":["ROMANIA","SAUDI ARABIA"]}},{"n_regionkey":{"$in":[3,4]}}]},{"n_name":{"$not":{"$in":["CHINA","ETHIOPIA","UNITED STATES","VIETNAM"]}}}]}}]
));
```
Subjoin plan:
```
COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"$and":[{"$or":[{"n_name":{"$in":["ROMANIA","SAUDI ARABIA"]}},{"n_regionkey":{"$in":[3,4]}}]},{"n_name":{"$not":{"$in":["CHINA","ETHIOPIA","UNITED STATES","VIETNAM"]}}}]}
```
Estimated cardinality: 10  
Actual cardinality: 10  
Orders of magnitude: 0

---
### >>> Subjoin 122-1
```
db.nation.aggregate(EJSON.deserialize(
[
{"$match":{"$and":[{"$or":[{"n_name":{"$in":["ROMANIA","SAUDI ARABIA"]}},{"n_regionkey":{"$in":[3,4]}}]},{"n_name":{"$not":{"$in":["CHINA","ETHIOPIA","UNITED STATES","VIETNAM"]}}}]}},
{"$lookup":{"from":"supplier","localField":"n_nationkey","foreignField":"s_nationkey","as":"supplier","pipeline":[
{"$match":{"$nor":[{"s_name":{"$eq":"Supplier#000000587"}},{"s_acctbal":{"$lte":6399.78}}]}}]}},
{"$unwind":"$supplier"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$supplier"]}}}]
));
```
Subjoin plan:
```
HJ n_nationkey = s_nationkey
  -> [nation_s] COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"$and":[{"$or":[{"n_name":{"$in":["ROMANIA","SAUDI ARABIA"]}},{"n_regionkey":{"$in":[3,4]}}]},{"n_name":{"$not":{"$in":["CHINA","ETHIOPIA","UNITED STATES","VIETNAM"]}}}]} 
  -> [supplier] COLLSCAN: plan_stability_subjoin_cardinality_md.supplier {"$nor":[{"s_name":{"$eq":"Supplier#000000587"}},{"s_acctbal":{"$lte":6399.78}}]}
```
Estimated cardinality: 129  
Actual cardinality: 125  
Orders of magnitude: 0

---
### >>> Subjoin 122-2
```
db.nation.aggregate(EJSON.deserialize(
[
{"$match":{"$and":[{"$or":[{"n_name":{"$in":["ROMANIA","SAUDI ARABIA"]}},{"n_regionkey":{"$in":[3,4]}}]},{"n_name":{"$not":{"$in":["CHINA","ETHIOPIA","UNITED STATES","VIETNAM"]}}}]}},
{"$lookup":{"from":"supplier","localField":"n_nationkey","foreignField":"s_nationkey","as":"supplier","pipeline":[
{"$match":{"$nor":[{"s_name":{"$eq":"Supplier#000000587"}},{"s_acctbal":{"$lte":6399.78}}]}}]}},
{"$unwind":"$supplier"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$supplier"]}}},
{"$lookup":{"from":"partsupp","localField":"s_suppkey","foreignField":"ps_suppkey","as":"partsupp","pipeline":[
{"$match":{"$and":[{"$and":[{"ps_availqty":{"$lt":7155}},{"ps_comment":{"$regex":"^s. "}},{"ps_comment":{"$not":{"$regex":"^ "}}}]},{}]}}]}},
{"$unwind":"$partsupp"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$partsupp"]}}}]
));
```
Subjoin plan:
```
INLJ supplier.s_suppkey = ps_suppkey
  -> [none] HJ n_nationkey = s_nationkey
      -> [nation_s] COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"$and":[{"$or":[{"n_name":{"$in":["ROMANIA","SAUDI ARABIA"]}},{"n_regionkey":{"$in":[3,4]}}]},{"n_name":{"$not":{"$in":["CHINA","ETHIOPIA","UNITED STATES","VIETNAM"]}}}]} 
      -> [supplier] COLLSCAN: plan_stability_subjoin_cardinality_md.supplier {"$nor":[{"s_name":{"$eq":"Supplier#000000587"}},{"s_acctbal":{"$lte":6399.78}}]} 
  -> [partsupp] FETCH: plan_stability_subjoin_cardinality_md.partsupp {"$and":[{"ps_availqty":{"$lt":7155}},{"ps_comment":{"$regex":"^s. "}},{"ps_comment":{"$not":{"$regex":"^ "}}}]} 
      -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.partsupp ps_suppkey_1
```
Estimated cardinality: 124  
Actual cardinality: 124  
Orders of magnitude: 0

---
### >>> Subjoin 122-3
```
db.nation.aggregate(EJSON.deserialize(
[
{"$match":{"$and":[{"$or":[{"n_name":{"$in":["ROMANIA","SAUDI ARABIA"]}},{"n_regionkey":{"$in":[3,4]}}]},{"n_name":{"$not":{"$in":["CHINA","ETHIOPIA","UNITED STATES","VIETNAM"]}}}]}},
{"$lookup":{"from":"supplier","localField":"n_nationkey","foreignField":"s_nationkey","as":"supplier","pipeline":[
{"$match":{"$nor":[{"s_name":{"$eq":"Supplier#000000587"}},{"s_acctbal":{"$lte":6399.78}}]}}]}},
{"$unwind":"$supplier"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$supplier"]}}},
{"$lookup":{"from":"partsupp","localField":"s_suppkey","foreignField":"ps_suppkey","as":"partsupp","pipeline":[
{"$match":{"$and":[{"$and":[{"ps_availqty":{"$lt":7155}},{"ps_comment":{"$regex":"^s. "}},{"ps_comment":{"$not":{"$regex":"^ "}}}]},{}]}}]}},
{"$unwind":"$partsupp"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$partsupp"]}}},
{"$lookup":{"from":"part","localField":"ps_partkey","foreignField":"p_partkey","as":"part","pipeline":[
{"$match":{"p_name":{"$regex":"^co"}}}]}},
{"$unwind":"$part"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$part"]}}}]
));
```
Subjoin plan:
```
HJ partsupp.ps_partkey = p_partkey
  -> [none] INLJ supplier.s_suppkey = ps_suppkey
      -> [none] HJ n_nationkey = s_nationkey
          -> [nation_s] COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"$and":[{"$or":[{"n_name":{"$in":["ROMANIA","SAUDI ARABIA"]}},{"n_regionkey":{"$in":[3,4]}}]},{"n_name":{"$not":{"$in":["CHINA","ETHIOPIA","UNITED STATES","VIETNAM"]}}}]} 
          -> [supplier] COLLSCAN: plan_stability_subjoin_cardinality_md.supplier {"$nor":[{"s_name":{"$eq":"Supplier#000000587"}},{"s_acctbal":{"$lte":6399.78}}]} 
      -> [partsupp] FETCH: plan_stability_subjoin_cardinality_md.partsupp {"$and":[{"ps_availqty":{"$lt":7155}},{"ps_comment":{"$regex":"^s. "}},{"ps_comment":{"$not":{"$regex":"^ "}}}]} 
          -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.partsupp ps_suppkey_1
  -> [none] COLLSCAN: plan_stability_subjoin_cardinality_md.part {"p_name":{"$regex":"^co"}}
```
Estimated cardinality: 4  
Actual cardinality: 6  
Orders of magnitude: 0

---
## >>> Command idx 123
```
{"aggregate":"supplier","pipeline":[
{"$lookup":{"from":"nation","localField":"s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$or":[{"n_name":"UNITED STATES"},{"n_name":"JORDAN"},{"n_name":"VIETNAM"}]}},
{"$match":{"$or":[{"n_name":"VIETNAM"},{"n_regionkey":4},{"n_regionkey":2}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$lookup":{"from":"region","localField":"nation_s.n_regionkey","foreignField":"r_regionkey","pipeline":[
{"$match":{"$or":[{"r_name":"AFRICA"},{"r_regionkey":4}]}}],"as":"region_s"}},
{"$unwind":"$region_s"},
{"$match":{"$nor":[{"region_s.r_name":"AFRICA"}]}}],"cursor":{},"idx":123}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 124
```
{"aggregate":"supplier","pipeline":[
{"$lookup":{"from":"nation","localField":"s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$or":[{"n_regionkey":4},{"n_name":"VIETNAM"}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$lookup":{"from":"region","localField":"nation_s.n_regionkey","foreignField":"r_regionkey","pipeline":[
{"$match":{"$nor":[{"r_name":"AMERICA"}]}}],"as":"region_s"}},
{"$unwind":"$region_s"},
{"$match":{"$or":[{"s_acctbal":{"$gte":8797.4}},{"s_name":"Supplier#000000676"}]}}],"cursor":{},"idx":124}
```
### >>> Subjoin 124-0
```
db.region.aggregate(EJSON.deserialize(
[
{"$match":{"r_name":{"$not":{"$eq":"AMERICA"}}}}]
));
```
Subjoin plan:
```
COLLSCAN: plan_stability_subjoin_cardinality_md.region {"r_name":{"$not":{"$eq":"AMERICA"}}}
```
Estimated cardinality: 4  
Actual cardinality: 4  
Orders of magnitude: 0

---
### >>> Subjoin 124-1
```
db.region.aggregate(EJSON.deserialize(
[
{"$match":{"r_name":{"$not":{"$eq":"AMERICA"}}}},
{"$lookup":{"from":"nation","localField":"r_regionkey","foreignField":"n_regionkey","as":"nation","pipeline":[
{"$match":{"$or":[{"n_name":{"$eq":"VIETNAM"}},{"n_regionkey":{"$eq":4}}]}}]}},
{"$unwind":"$nation"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$nation"]}}}]
));
```
Subjoin plan:
```
HJ r_regionkey = n_regionkey
  -> [region_s] COLLSCAN: plan_stability_subjoin_cardinality_md.region {"r_name":{"$not":{"$eq":"AMERICA"}}} 
  -> [nation_s] COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"$or":[{"n_name":{"$eq":"VIETNAM"}},{"n_regionkey":{"$eq":4}}]}
```
Estimated cardinality: 5  
Actual cardinality: 6  
Orders of magnitude: 0

---
### >>> Subjoin 124-2
```
db.region.aggregate(EJSON.deserialize(
[
{"$match":{"r_name":{"$not":{"$eq":"AMERICA"}}}},
{"$lookup":{"from":"nation","localField":"r_regionkey","foreignField":"n_regionkey","as":"nation","pipeline":[
{"$match":{"$or":[{"n_name":{"$eq":"VIETNAM"}},{"n_regionkey":{"$eq":4}}]}}]}},
{"$unwind":"$nation"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$nation"]}}},
{"$lookup":{"from":"supplier","localField":"n_nationkey","foreignField":"s_nationkey","as":"supplier","pipeline":[
{"$match":{"$or":[{"s_name":{"$eq":"Supplier#000000676"}},{"s_acctbal":{"$gte":8797.4}}]}}]}},
{"$unwind":"$supplier"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$supplier"]}}}]
));
```
Subjoin plan:
```
HJ nation_s.n_nationkey = s_nationkey
  -> [none] HJ r_regionkey = n_regionkey
      -> [region_s] COLLSCAN: plan_stability_subjoin_cardinality_md.region {"r_name":{"$not":{"$eq":"AMERICA"}}} 
      -> [nation_s] COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"$or":[{"n_name":{"$eq":"VIETNAM"}},{"n_regionkey":{"$eq":4}}]} 
  -> [none] COLLSCAN: plan_stability_subjoin_cardinality_md.supplier {"$or":[{"s_name":{"$eq":"Supplier#000000676"}},{"s_acctbal":{"$gte":8797.4}}]}
```
Estimated cardinality: 23  
Actual cardinality: 26  
Orders of magnitude: 0

---
## >>> Command idx 125
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$or":[{"ps_supplycost":{"$eq":27.55}},{"ps_comment":{"$regex":{"$regex":"^ pa","$options":""}}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"supplier","localField":"partsupp.ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$or":[{"s_acctbal":{"$gt":1578.18}},{"s_name":"Supplier#000000008"},{"s_acctbal":{"$eq":8512.48}}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$match":{"$nor":[{"p_partkey":4487},{"supplier.o_shippriority":{"$gte":0}}]}}],"cursor":{},"idx":125}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 126
```
{"aggregate":"lineitem","pipeline":[
{"$lookup":{"from":"orders","localField":"l_orderkey","foreignField":"o_orderkey","pipeline":[
{"$match":{"$or":[{"o_shippriority":{"$gt":0}},{"o_totalprice":{"$lt":37208.13}}]}},
{"$match":{"$or":[{"o_totalprice":{"$lt":141506.52}},{"o_shippriority":{"$gt":0}}]}}],"as":"orders"}},
{"$unwind":"$orders"},
{"$lookup":{"from":"customer","localField":"orders.o_custkey","foreignField":"c_custkey","pipeline":[
{"$match":{"$nor":[{"c_mktsegment":"AUTOMOBILE"},{"c_mktsegment":"AUTOMOBILE"}]}}],"as":"customer"}},
{"$unwind":"$customer"},
{"$lookup":{"from":"supplier","localField":"customer.c_nationkey","foreignField":"s_nationkey","pipeline":[
{"$match":{"$and":[{"s_acctbal":{"$eq":5704.81}}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$match":{"$and":[{"customer.c_acctbal":{"$lte":2998.55}},{"l_shipmode":"RAIL"}]}}],"cursor":{},"idx":126}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 127
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$and":[{"ps_comment":{"$regex":{"$regex":"^ar","$options":""}}},{"ps_availqty":{"$lte":7831}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"supplier","localField":"partsupp.ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$and":[{"s_acctbal":{"$gt":1209.3}},{"s_acctbal":{"$gte":1432.69}}]}},
{"$match":{"$and":[{"s_nationkey":9}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$match":{"$nor":[{"supplier.s_acctbal":{"$eq":3222.71}},{"p_size":{"$gt":40}}]}}],"cursor":{},"idx":127}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 128
```
{"aggregate":"supplier","pipeline":[
{"$lookup":{"from":"nation","localField":"s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$nor":[{"n_regionkey":2},{"n_name":"EGYPT"},{"n_regionkey":4}]}},
{"$match":{"$nor":[{"n_name":"ARGENTINA"},{"n_regionkey":4}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$lookup":{"from":"region","localField":"nation_s.n_regionkey","foreignField":"r_regionkey","pipeline":[
{"$match":{"$or":[{"r_name":"EUROPE"}]}}],"as":"region_s"}},
{"$unwind":"$region_s"},
{"$match":{"$or":[{"s_name":"Supplier#000000085"},{"s_name":"Supplier#000000454"}]}}],"cursor":{},"idx":128}
```
### >>> Subjoin 128-0
```
db.supplier.aggregate(EJSON.deserialize(
[
{"$match":{"s_name":{"$in":["Supplier#000000085","Supplier#000000454"]}}}]
));
```
Subjoin plan:
```
COLLSCAN: plan_stability_subjoin_cardinality_md.supplier {"s_name":{"$in":["Supplier#000000085","Supplier#000000454"]}}
```
Estimated cardinality: 2  
Actual cardinality: 2  
Orders of magnitude: 0

---
### >>> Subjoin 128-1
```
db.supplier.aggregate(EJSON.deserialize(
[
{"$match":{"s_name":{"$in":["Supplier#000000085","Supplier#000000454"]}}},
{"$lookup":{"from":"nation","localField":"s_nationkey","foreignField":"n_nationkey","as":"nation","pipeline":[
{"$match":{"$and":[{"$nor":[{"n_name":{"$eq":"ARGENTINA"}},{"n_regionkey":{"$eq":4}}]},{"$nor":[{"n_name":{"$eq":"EGYPT"}},{"n_regionkey":{"$eq":2}},{"n_regionkey":{"$eq":4}}]}]}}]}},
{"$unwind":"$nation"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$nation"]}}}]
));
```
Subjoin plan:
```
HJ s_nationkey = n_nationkey
  -> [none] COLLSCAN: plan_stability_subjoin_cardinality_md.supplier {"s_name":{"$in":["Supplier#000000085","Supplier#000000454"]}} 
  -> [nation_s] COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"$and":[{"$nor":[{"n_name":{"$eq":"ARGENTINA"}},{"n_regionkey":{"$eq":4}}]},{"$nor":[{"n_name":{"$eq":"EGYPT"}},{"n_regionkey":{"$eq":2}},{"n_regionkey":{"$eq":4}}]}]}
```
Estimated cardinality: 1  
Actual cardinality: 2  
Orders of magnitude: 0

---
### >>> Subjoin 128-2
```
db.supplier.aggregate(EJSON.deserialize(
[
{"$match":{"s_name":{"$in":["Supplier#000000085","Supplier#000000454"]}}},
{"$lookup":{"from":"nation","localField":"s_nationkey","foreignField":"n_nationkey","as":"nation","pipeline":[
{"$match":{"$and":[{"$nor":[{"n_name":{"$eq":"ARGENTINA"}},{"n_regionkey":{"$eq":4}}]},{"$nor":[{"n_name":{"$eq":"EGYPT"}},{"n_regionkey":{"$eq":2}},{"n_regionkey":{"$eq":4}}]}]}}]}},
{"$unwind":"$nation"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$nation"]}}},
{"$lookup":{"from":"region","localField":"n_regionkey","foreignField":"r_regionkey","as":"region","pipeline":[
{"$match":{"r_name":{"$eq":"EUROPE"}}}]}},
{"$unwind":"$region"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$region"]}}}]
));
```
Subjoin plan:
```
HJ r_regionkey = nation_s.n_regionkey
  -> [region_s] COLLSCAN: plan_stability_subjoin_cardinality_md.region {"r_name":{"$eq":"EUROPE"}} 
  -> [none] HJ s_nationkey = n_nationkey
      -> [none] COLLSCAN: plan_stability_subjoin_cardinality_md.supplier {"s_name":{"$in":["Supplier#000000085","Supplier#000000454"]}} 
      -> [nation_s] COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"$and":[{"$nor":[{"n_name":{"$eq":"ARGENTINA"}},{"n_regionkey":{"$eq":4}}]},{"$nor":[{"n_name":{"$eq":"EGYPT"}},{"n_regionkey":{"$eq":2}},{"n_regionkey":{"$eq":4}}]}]}
```
Estimated cardinality: 0  
Actual cardinality: 1  
Orders of magnitude: 0

---
## >>> Command idx 129
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$nor":[{"ps_supplycost":{"$gte":47.26}},{"ps_supplycost":{"$gt":355.65}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"lineitem","localField":"partsupp.ps_partkey","foreignField":"l_partkey","pipeline":[
{"$match":{"$and":[{"l_tax":{"$lte":0.01}}]}},
{"$match":{"$or":[{"l_commitdate":{"$eq":"1993-03-17T00:00:00.000Z"}}]}}],"as":"lineitem"}},
{"$unwind":"$lineitem"},
{"$match":{"$nor":[{"p_mfgr":"Manufacturer#2"},{"lineitem.l_shipinstruct":"NONE"}]}}],"cursor":{},"idx":129}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 130
```
{"aggregate":"partsupp","pipeline":[
{"$lookup":{"from":"supplier","localField":"ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$nor":[{"s_acctbal":{"$gt":7182.24}},{"s_nationkey":20},{"s_acctbal":{"$gte":-128.86}}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$nor":[{"n_name":"JAPAN"},{"n_name":"FRANCE"}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$match":{"$nor":[{"nation_s.n_name":"CANADA"},{"nation_s.n_name":{"$nin":["ARGENTINA","SAUDI ARABIA"]}},{"ps_availqty":{"$eq":6133}}]}}],"cursor":{},"idx":130}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 131
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$or":[{"ps_supplycost":{"$lte":17.49}}]}},
{"$match":{"$nor":[{"ps_supplycost":{"$gt":944.57}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"lineitem","localField":"partsupp.ps_partkey","foreignField":"l_partkey","pipeline":[
{"$match":{"$and":[{"l_discount":{"$gte":0.02}}]}},
{"$match":{"$nor":[{"l_linenumber":{"$lt":5}},{"l_linestatus":"O"}]}}],"as":"lineitem"}},
{"$unwind":"$lineitem"},
{"$match":{"$and":[{"p_comment":{"$regex":{"$regex":"^li","$options":""}}}]}}],"cursor":{},"idx":131}
```
### >>> Subjoin 131-0
```
db.part.aggregate(EJSON.deserialize(
[
{"$match":{"p_comment":{"$regex":"^li"}}}]
));
```
Subjoin plan:
```
COLLSCAN: plan_stability_subjoin_cardinality_md.part {"p_comment":{"$regex":"^li"}}
```
Estimated cardinality: 160  
Actual cardinality: 132  
Orders of magnitude: 0

---
### >>> Subjoin 131-1
```
db.part.aggregate(EJSON.deserialize(
[
{"$match":{"p_comment":{"$regex":"^li"}}},
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","as":"partsupp","pipeline":[
{"$match":{"$and":[{"$and":[{"ps_supplycost":{"$lte":17.49}},{"ps_supplycost":{"$not":{"$gt":944.57}}}]},{}]}}]}},
{"$unwind":"$partsupp"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$partsupp"]}}}]
));
```
Subjoin plan:
```
INLJ p_partkey = ps_partkey
  -> [none] COLLSCAN: plan_stability_subjoin_cardinality_md.part {"p_comment":{"$regex":"^li"}} 
  -> [partsupp] FETCH: plan_stability_subjoin_cardinality_md.partsupp {"$and":[{"ps_supplycost":{"$lte":17.49}},{"ps_supplycost":{"$not":{"$gt":944.57}}}]} 
      -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.partsupp ps_partkey_1
```
Estimated cardinality: 6  
Actual cardinality: 10  
Orders of magnitude: 1

---
### >>> Subjoin 131-2
```
db.part.aggregate(EJSON.deserialize(
[
{"$match":{"p_comment":{"$regex":"^li"}}},
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","as":"partsupp","pipeline":[
{"$match":{"$and":[{"$and":[{"ps_supplycost":{"$lte":17.49}},{"ps_supplycost":{"$not":{"$gt":944.57}}}]},{}]}}]}},
{"$unwind":"$partsupp"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$partsupp"]}}},
{"$lookup":{"from":"lineitem","localField":"p_partkey","foreignField":"l_partkey","as":"lineitem","pipeline":[
{"$match":{"$and":[{"$and":[{"l_discount":{"$gte":0.02}},{"$nor":[{"l_linestatus":{"$eq":"O"}},{"l_linenumber":{"$lt":5}}]}]},{}]}}]}},
{"$unwind":"$lineitem"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$lineitem"]}}}]
));
```
Subjoin plan:
```
INLJ partsupp.ps_partkey = l_partkey, p_partkey = l_partkey
  -> [none] INLJ p_partkey = ps_partkey
      -> [none] COLLSCAN: plan_stability_subjoin_cardinality_md.part {"p_comment":{"$regex":"^li"}} 
      -> [partsupp] FETCH: plan_stability_subjoin_cardinality_md.partsupp {"$and":[{"ps_supplycost":{"$lte":17.49}},{"ps_supplycost":{"$not":{"$gt":944.57}}}]} 
          -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.partsupp ps_partkey_1
  -> [lineitem] FETCH: plan_stability_subjoin_cardinality_md.lineitem {"$and":[{"l_discount":{"$gte":0.02}},{"$nor":[{"l_linestatus":{"$eq":"O"}},{"l_linenumber":{"$lt":5}}]}]} 
      -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.lineitem l_partkey_1
```
Estimated cardinality: 1166  
Actual cardinality: 20  
Orders of magnitude: 2
> [!WARNING]
> Estimate discrepancy is more than 2 orders of magnitude.

---
## >>> Command idx 132
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$and":[{"ps_comment":{"$regex":{"$regex":"^s. ","$options":""}}}]}},
{"$match":{"$nor":[{"ps_supplycost":{"$gt":705.79}},{"ps_comment":{"$regex":{"$regex":"^req","$options":""}}},{"ps_availqty":{"$lte":758}}]}},
{"$match":{"$nor":[{"ps_comment":{"$regex":{"$regex":"^. ","$options":""}}},{"ps_comment":{"$regex":{"$regex":"^ea","$options":""}}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"supplier","localField":"partsupp.ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$nor":[{"s_name":"Supplier#000000556"}]}},
{"$match":{"$nor":[{"s_name":"Supplier#000000046"}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$match":{"$nor":[{"supplier.s_nationkey":9},{"p_retailprice":{"$gte":1103.19}}]}}],"cursor":{},"idx":132}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 133
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$or":[{"ps_comment":{"$regex":{"$regex":"^b","$options":""}}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"supplier","localField":"partsupp.ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$and":[{"s_acctbal":{"$gte":2800.6}}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$nor":[{"n_name":"UNITED STATES"},{"n_name":"CHINA"}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$match":{"$nor":[{"p_brand":"Brand#15"},{"supplier.o_clerk":"Clerk#000000062"},{"supplier.o_orderpriority":"1-URGENT"},{"p_mfgr":{"$in":["Manufacturer#3","Manufacturer#1"]}}]}}],"cursor":{},"idx":133}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 134
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$or":[{"ps_supplycost":{"$lt":892.51}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"supplier","localField":"partsupp.ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$or":[{"s_acctbal":{"$lt":958.07}}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$match":{"$or":[{"p_comment":{"$regex":{"$regex":"^x","$options":""}}}]}}],"cursor":{},"idx":134}
```
### >>> Subjoin 134-0
```
db.part.aggregate(EJSON.deserialize(
[
{"$match":{"p_comment":{"$regex":"^x"}}}]
));
```
Subjoin plan:
```
COLLSCAN: plan_stability_subjoin_cardinality_md.part {"p_comment":{"$regex":"^x"}}
```
Estimated cardinality: 120  
Actual cardinality: 119  
Orders of magnitude: 0

---
### >>> Subjoin 134-1
```
db.part.aggregate(EJSON.deserialize(
[
{"$match":{"p_comment":{"$regex":"^x"}}},
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","as":"partsupp","pipeline":[
{"$match":{"$and":[{"ps_supplycost":{"$lt":892.51}},{}]}}]}},
{"$unwind":"$partsupp"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$partsupp"]}}}]
));
```
Subjoin plan:
```
INLJ p_partkey = ps_partkey
  -> [none] COLLSCAN: plan_stability_subjoin_cardinality_md.part {"p_comment":{"$regex":"^x"}} 
  -> [partsupp] FETCH: plan_stability_subjoin_cardinality_md.partsupp {"ps_supplycost":{"$lt":892.51}} 
      -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.partsupp ps_partkey_1
```
Estimated cardinality: 431  
Actual cardinality: 430  
Orders of magnitude: 0

---
### >>> Subjoin 134-2
```
db.part.aggregate(EJSON.deserialize(
[
{"$match":{"p_comment":{"$regex":"^x"}}},
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","as":"partsupp","pipeline":[
{"$match":{"$and":[{"ps_supplycost":{"$lt":892.51}},{}]}}]}},
{"$unwind":"$partsupp"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$partsupp"]}}},
{"$lookup":{"from":"supplier","localField":"ps_suppkey","foreignField":"s_suppkey","as":"supplier","pipeline":[
{"$match":{"s_acctbal":{"$lt":958.07}}}]}},
{"$unwind":"$supplier"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$supplier"]}}}]
));
```
Subjoin plan:
```
HJ s_suppkey = partsupp.ps_suppkey
  -> [supplier] COLLSCAN: plan_stability_subjoin_cardinality_md.supplier {"s_acctbal":{"$lt":958.07}} 
  -> [none] INLJ p_partkey = ps_partkey
      -> [none] COLLSCAN: plan_stability_subjoin_cardinality_md.part {"p_comment":{"$regex":"^x"}} 
      -> [partsupp] FETCH: plan_stability_subjoin_cardinality_md.partsupp {"ps_supplycost":{"$lt":892.51}} 
          -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.partsupp ps_partkey_1
```
Estimated cardinality: 78  
Actual cardinality: 74  
Orders of magnitude: 0

---
## >>> Command idx 135
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$and":[{"ps_comment":{"$regex":{"$regex":"^ f","$options":""}}}]}},
{"$match":{"$or":[{"ps_comment":{"$regex":{"$regex":"^lu","$options":""}}},{"ps_supplycost":{"$lt":240.39}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"lineitem","localField":"partsupp.ps_partkey","foreignField":"l_partkey","pipeline":[
{"$match":{"$and":[{"l_shipinstruct":"DELIVER IN PERSON"},{"l_discount":{"$lte":0.09}}]}},
{"$match":{"$nor":[{"l_tax":{"$lte":0.06}}]}}],"as":"lineitem"}},
{"$unwind":"$lineitem"},
{"$match":{"$and":[{"p_container":{"$in":["WRAP PACK","LG CAN","LG CAN","WRAP CAN"]}},{"lineitem.l_shipmode":{"$in":["REG AIR","RAIL","RAIL","RAIL"]}}]}}],"cursor":{},"idx":135}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 136
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$and":[{"ps_availqty":{"$lt":9209}}]}},
{"$match":{"$and":[{"ps_supplycost":{"$lt":255.03}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"supplier","localField":"partsupp.ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$or":[{"s_acctbal":{"$gte":8797.4}},{"s_name":"Supplier#000000924"}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$or":[{"n_name":{"$in":["UNITED KINGDOM","ARGENTINA","ROMANIA"]}},{"n_regionkey":2}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$lookup":{"from":"region","localField":"nation_s.n_regionkey","foreignField":"r_regionkey","pipeline":[
{"$match":{"$or":[{"r_name":"ASIA"},{"r_regionkey":0}]}}],"as":"region_s"}},
{"$unwind":"$region_s"},
{"$match":{"$and":[{"p_comment":{"$regex":{"$regex":"^egu","$options":""}}}]}}],"cursor":{},"idx":136}
```
### >>> Subjoin 136-0
```
db.region.aggregate(EJSON.deserialize(
[
{"$match":{"$or":[{"r_name":{"$eq":"ASIA"}},{"r_regionkey":{"$eq":0}}]}}]
));
```
Subjoin plan:
```
COLLSCAN: plan_stability_subjoin_cardinality_md.region {"$or":[{"r_name":{"$eq":"ASIA"}},{"r_regionkey":{"$eq":0}}]}
```
Estimated cardinality: 2  
Actual cardinality: 2  
Orders of magnitude: 0

---
### >>> Subjoin 136-1
```
db.region.aggregate(EJSON.deserialize(
[
{"$match":{"$or":[{"r_name":{"$eq":"ASIA"}},{"r_regionkey":{"$eq":0}}]}},
{"$lookup":{"from":"nation","localField":"r_regionkey","foreignField":"n_regionkey","as":"nation","pipeline":[
{"$match":{"$or":[{"n_regionkey":{"$eq":2}},{"n_name":{"$in":["ARGENTINA","ROMANIA","UNITED KINGDOM"]}}]}}]}},
{"$unwind":"$nation"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$nation"]}}}]
));
```
Subjoin plan:
```
HJ r_regionkey = n_regionkey
  -> [region_s] COLLSCAN: plan_stability_subjoin_cardinality_md.region {"$or":[{"r_name":{"$eq":"ASIA"}},{"r_regionkey":{"$eq":0}}]} 
  -> [nation_s] COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"$or":[{"n_regionkey":{"$eq":2}},{"n_name":{"$in":["ARGENTINA","ROMANIA","UNITED KINGDOM"]}}]}
```
Estimated cardinality: 3  
Actual cardinality: 5  
Orders of magnitude: 0

---
### >>> Subjoin 136-2
```
db.region.aggregate(EJSON.deserialize(
[
{"$match":{"$or":[{"r_name":{"$eq":"ASIA"}},{"r_regionkey":{"$eq":0}}]}},
{"$lookup":{"from":"nation","localField":"r_regionkey","foreignField":"n_regionkey","as":"nation","pipeline":[
{"$match":{"$or":[{"n_regionkey":{"$eq":2}},{"n_name":{"$in":["ARGENTINA","ROMANIA","UNITED KINGDOM"]}}]}}]}},
{"$unwind":"$nation"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$nation"]}}},
{"$lookup":{"from":"supplier","localField":"n_nationkey","foreignField":"s_nationkey","as":"supplier","pipeline":[
{"$match":{"$or":[{"s_name":{"$eq":"Supplier#000000924"}},{"s_acctbal":{"$gte":8797.4}}]}}]}},
{"$unwind":"$supplier"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$supplier"]}}}]
));
```
Subjoin plan:
```
HJ nation_s.n_nationkey = s_nationkey
  -> [none] HJ r_regionkey = n_regionkey
      -> [region_s] COLLSCAN: plan_stability_subjoin_cardinality_md.region {"$or":[{"r_name":{"$eq":"ASIA"}},{"r_regionkey":{"$eq":0}}]} 
      -> [nation_s] COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"$or":[{"n_regionkey":{"$eq":2}},{"n_name":{"$in":["ARGENTINA","ROMANIA","UNITED KINGDOM"]}}]} 
  -> [supplier] COLLSCAN: plan_stability_subjoin_cardinality_md.supplier {"$or":[{"s_name":{"$eq":"Supplier#000000924"}},{"s_acctbal":{"$gte":8797.4}}]}
```
Estimated cardinality: 15  
Actual cardinality: 30  
Orders of magnitude: 0

---
### >>> Subjoin 136-3
```
db.region.aggregate(EJSON.deserialize(
[
{"$match":{"$or":[{"r_name":{"$eq":"ASIA"}},{"r_regionkey":{"$eq":0}}]}},
{"$lookup":{"from":"nation","localField":"r_regionkey","foreignField":"n_regionkey","as":"nation","pipeline":[
{"$match":{"$or":[{"n_regionkey":{"$eq":2}},{"n_name":{"$in":["ARGENTINA","ROMANIA","UNITED KINGDOM"]}}]}}]}},
{"$unwind":"$nation"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$nation"]}}},
{"$lookup":{"from":"supplier","localField":"n_nationkey","foreignField":"s_nationkey","as":"supplier","pipeline":[
{"$match":{"$or":[{"s_name":{"$eq":"Supplier#000000924"}},{"s_acctbal":{"$gte":8797.4}}]}}]}},
{"$unwind":"$supplier"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$supplier"]}}},
{"$lookup":{"from":"partsupp","localField":"s_suppkey","foreignField":"ps_suppkey","as":"partsupp","pipeline":[
{"$match":{"$and":[{"$and":[{"ps_availqty":{"$lt":9209}},{"ps_supplycost":{"$lt":255.03}}]},{}]}}]}},
{"$unwind":"$partsupp"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$partsupp"]}}}]
));
```
Subjoin plan:
```
INLJ supplier.s_suppkey = ps_suppkey
  -> [none] HJ nation_s.n_nationkey = s_nationkey
      -> [none] HJ r_regionkey = n_regionkey
          -> [region_s] COLLSCAN: plan_stability_subjoin_cardinality_md.region {"$or":[{"r_name":{"$eq":"ASIA"}},{"r_regionkey":{"$eq":0}}]} 
          -> [nation_s] COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"$or":[{"n_regionkey":{"$eq":2}},{"n_name":{"$in":["ARGENTINA","ROMANIA","UNITED KINGDOM"]}}]} 
      -> [supplier] COLLSCAN: plan_stability_subjoin_cardinality_md.supplier {"$or":[{"s_name":{"$eq":"Supplier#000000924"}},{"s_acctbal":{"$gte":8797.4}}]} 
  -> [partsupp] FETCH: plan_stability_subjoin_cardinality_md.partsupp {"$and":[{"ps_availqty":{"$lt":9209}},{"ps_supplycost":{"$lt":255.03}}]} 
      -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.partsupp ps_suppkey_1
```
Estimated cardinality: 273  
Actual cardinality: 587  
Orders of magnitude: 0

---
### >>> Subjoin 136-4
```
db.region.aggregate(EJSON.deserialize(
[
{"$match":{"$or":[{"r_name":{"$eq":"ASIA"}},{"r_regionkey":{"$eq":0}}]}},
{"$lookup":{"from":"nation","localField":"r_regionkey","foreignField":"n_regionkey","as":"nation","pipeline":[
{"$match":{"$or":[{"n_regionkey":{"$eq":2}},{"n_name":{"$in":["ARGENTINA","ROMANIA","UNITED KINGDOM"]}}]}}]}},
{"$unwind":"$nation"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$nation"]}}},
{"$lookup":{"from":"supplier","localField":"n_nationkey","foreignField":"s_nationkey","as":"supplier","pipeline":[
{"$match":{"$or":[{"s_name":{"$eq":"Supplier#000000924"}},{"s_acctbal":{"$gte":8797.4}}]}}]}},
{"$unwind":"$supplier"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$supplier"]}}},
{"$lookup":{"from":"partsupp","localField":"s_suppkey","foreignField":"ps_suppkey","as":"partsupp","pipeline":[
{"$match":{"$and":[{"$and":[{"ps_availqty":{"$lt":9209}},{"ps_supplycost":{"$lt":255.03}}]},{}]}}]}},
{"$unwind":"$partsupp"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$partsupp"]}}},
{"$lookup":{"from":"part","localField":"ps_partkey","foreignField":"p_partkey","as":"part","pipeline":[
{"$match":{"p_comment":{"$regex":"^egu"}}}]}},
{"$unwind":"$part"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$part"]}}}]
));
```
Subjoin plan:
```
HJ p_partkey = partsupp.ps_partkey
  -> [none] COLLSCAN: plan_stability_subjoin_cardinality_md.part {"p_comment":{"$regex":"^egu"}} 
  -> [none] INLJ supplier.s_suppkey = ps_suppkey
      -> [none] HJ nation_s.n_nationkey = s_nationkey
          -> [none] HJ r_regionkey = n_regionkey
              -> [region_s] COLLSCAN: plan_stability_subjoin_cardinality_md.region {"$or":[{"r_name":{"$eq":"ASIA"}},{"r_regionkey":{"$eq":0}}]} 
              -> [nation_s] COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"$or":[{"n_regionkey":{"$eq":2}},{"n_name":{"$in":["ARGENTINA","ROMANIA","UNITED KINGDOM"]}}]} 
          -> [supplier] COLLSCAN: plan_stability_subjoin_cardinality_md.supplier {"$or":[{"s_name":{"$eq":"Supplier#000000924"}},{"s_acctbal":{"$gte":8797.4}}]} 
      -> [partsupp] FETCH: plan_stability_subjoin_cardinality_md.partsupp {"$and":[{"ps_availqty":{"$lt":9209}},{"ps_supplycost":{"$lt":255.03}}]} 
          -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.partsupp ps_suppkey_1
```
Estimated cardinality: 1  
Actual cardinality: 5  
Orders of magnitude: 0

---
## >>> Command idx 137
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$or":[{"ps_supplycost":{"$lt":161.52}},{"ps_comment":{"$regex":{"$regex":"^y e","$options":""}}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"supplier","localField":"partsupp.ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$or":[{"s_acctbal":{"$lt":6835.16}},{"s_name":"Supplier#000000349"},{"s_acctbal":{"$gte":5302.37}}]}},
{"$match":{"$nor":[{"s_acctbal":{"$gte":8724.42}},{"s_nationkey":22}]}},
{"$match":{"$or":[{"s_acctbal":{"$gt":7162.15}},{"s_nationkey":19}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$or":[{"n_name":{"$in":["ARGENTINA","PERU"]}},{"n_name":{"$in":["INDIA","BRAZIL","BRAZIL"]}}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$match":{"$nor":[{"supplier.o_orderdate":{"$lte":"1992-01-24T00:00:00.000Z"}},{"nation_s.n_regionkey":3},{"p_container":{"$in":["JUMBO JAR","MED JAR","MED CASE"]}}]}}],"cursor":{},"idx":137}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 138
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$or":[{"ps_comment":{"$regex":{"$regex":"^aref","$options":""}}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"supplier","localField":"partsupp.ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$or":[{"s_acctbal":{"$lte":5704.81}}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$match":{"$nor":[{"p_size":{"$eq":15}}]}}],"cursor":{},"idx":138}
```
### >>> Subjoin 138-0
```
db.partsupp.aggregate(EJSON.deserialize(
[
{"$match":{"ps_comment":{"$regex":"^aref"}}}]
));
```
Subjoin plan:
```
COLLSCAN: plan_stability_subjoin_cardinality_md.partsupp {"ps_comment":{"$regex":"^aref"}}
```
Estimated cardinality: 560  
Actual cardinality: 399  
Orders of magnitude: 0

---
### >>> Subjoin 138-1
```
db.partsupp.aggregate(EJSON.deserialize(
[
{"$match":{"ps_comment":{"$regex":"^aref"}}},
{"$lookup":{"from":"supplier","localField":"ps_suppkey","foreignField":"s_suppkey","as":"supplier","pipeline":[
{"$match":{"s_acctbal":{"$lte":5704.81}}}]}},
{"$unwind":"$supplier"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$supplier"]}}}]
));
```
Subjoin plan:
```
HJ ps_suppkey = s_suppkey
  -> [partsupp] COLLSCAN: plan_stability_subjoin_cardinality_md.partsupp {"ps_comment":{"$regex":"^aref"}} 
  -> [supplier] COLLSCAN: plan_stability_subjoin_cardinality_md.supplier {"s_acctbal":{"$lte":5704.81}}
```
Estimated cardinality: 338  
Actual cardinality: 231  
Orders of magnitude: 0

---
### >>> Subjoin 138-2
```
db.partsupp.aggregate(EJSON.deserialize(
[
{"$match":{"ps_comment":{"$regex":"^aref"}}},
{"$lookup":{"from":"supplier","localField":"ps_suppkey","foreignField":"s_suppkey","as":"supplier","pipeline":[
{"$match":{"s_acctbal":{"$lte":5704.81}}}]}},
{"$unwind":"$supplier"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$supplier"]}}},
{"$lookup":{"from":"part","localField":"ps_partkey","foreignField":"p_partkey","as":"part","pipeline":[
{"$match":{"p_size":{"$not":{"$eq":15}}}}]}},
{"$unwind":"$part"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$part"]}}}]
));
```
Subjoin plan:
```
HJ partsupp.ps_partkey = p_partkey
  -> [none] HJ ps_suppkey = s_suppkey
      -> [partsupp] COLLSCAN: plan_stability_subjoin_cardinality_md.partsupp {"ps_comment":{"$regex":"^aref"}} 
      -> [supplier] COLLSCAN: plan_stability_subjoin_cardinality_md.supplier {"s_acctbal":{"$lte":5704.81}} 
  -> [none] COLLSCAN: plan_stability_subjoin_cardinality_md.part {"p_size":{"$not":{"$eq":15}}}
```
Estimated cardinality: 333  
Actual cardinality: 222  
Orders of magnitude: 0

---
## >>> Command idx 139
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$and":[{"ps_availqty":{"$gt":3160}},{"ps_availqty":{"$gt":9530}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"supplier","localField":"partsupp.ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$or":[{"s_name":"Supplier#000000836"}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$nor":[{"n_regionkey":0}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$match":{"$nor":[{"supplier.o_orderpriority":"5-LOW"},{"p_type":"LARGE POLISHED COPPER"}]}}],"cursor":{},"idx":139}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 140
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$nor":[{"ps_comment":{"$regex":{"$regex":"^en","$options":""}}},{"ps_availqty":{"$lte":9209}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"supplier","localField":"partsupp.ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$or":[{"s_name":"Supplier#000000225"},{"s_nationkey":0},{"s_acctbal":{"$gt":5046.81}},{"s_name":"Supplier#000000469"}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$or":[{"n_name":"UNITED KINGDOM"},{"n_name":{"$nin":["MOZAMBIQUE","IRAN","FRANCE"]}}]}},
{"$match":{"$or":[{"n_name":{"$in":["SAUDI ARABIA","JORDAN","SAUDI ARABIA"]}},{"n_regionkey":0}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$lookup":{"from":"region","localField":"nation_s.n_regionkey","foreignField":"r_regionkey","pipeline":[
{"$match":{"$nor":[{"r_regionkey":3},{"r_name":"EUROPE"}]}},
{"$match":{"$nor":[{"r_name":"ASIA"}]}},
{"$match":{"$nor":[{"r_regionkey":4}]}}],"as":"region_s"}},
{"$unwind":"$region_s"},
{"$match":{"$nor":[{"p_name":"blanched ghost cream azure tomato"},{"nation_s.n_regionkey":3}]}}],"cursor":{},"idx":140}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 141
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$and":[{"ps_supplycost":{"$lte":836.01}},{"ps_supplycost":{"$gt":498.13}},{"ps_comment":{"$regex":{"$regex":"^b","$options":""}}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"supplier","localField":"partsupp.ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$or":[{"s_nationkey":24},{"s_acctbal":{"$gt":7082.37}}]}},
{"$match":{"$or":[{"s_name":"Supplier#000000602"},{"s_name":"Supplier#000000928"},{"s_acctbal":{"$lt":8724.42}}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$or":[{"n_regionkey":1},{"n_name":"MOROCCO"},{"n_regionkey":2},{"n_name":{"$in":["CHINA","EGYPT"]}}]}},
{"$match":{"$or":[{"n_regionkey":2}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$match":{"$nor":[{"nation_s.n_regionkey":4},{"p_type":"STANDARD POLISHED BRASS"}]}}],"cursor":{},"idx":141}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 142
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$nor":[{"ps_supplycost":{"$lte":485.1}}]}},
{"$match":{"$nor":[{"ps_availqty":{"$lt":9913}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"lineitem","localField":"partsupp.ps_partkey","foreignField":"l_partkey","pipeline":[
{"$match":{"$nor":[{"l_orderkey":555271}]}}],"as":"lineitem"}},
{"$unwind":"$lineitem"},
{"$match":{"$and":[{"p_comment":{"$regex":{"$regex":"^e","$options":""}}},{"lineitem.l_shipmode":{"$in":["TRUCK","REG AIR"]}}]}}],"cursor":{},"idx":142}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 143
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$and":[{"ps_comment":{"$regex":{"$regex":"^y ","$options":""}}},{"ps_supplycost":{"$lt":947.21}}]}},
{"$match":{"$and":[{"ps_availqty":{"$lt":5779}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"supplier","localField":"partsupp.ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$or":[{"s_acctbal":{"$lt":868.36}}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$or":[{"n_name":"INDIA"},{"n_regionkey":1},{"n_name":"JAPAN"}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$match":{"$nor":[{"p_retailprice":{"$lte":1404.49}},{"supplier.o_orderpriority":"3-MEDIUM"}]}}],"cursor":{},"idx":143}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 144
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$nor":[{"ps_availqty":{"$eq":4022}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"supplier","localField":"partsupp.ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$or":[{"s_acctbal":{"$lt":-111.84}},{"s_name":"Supplier#000000119"}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$or":[{"n_regionkey":4},{"n_regionkey":0}]}},
{"$match":{"$or":[{"n_name":{"$in":["JAPAN","ETHIOPIA"]}}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$match":{"$nor":[{"p_comment":{"$regex":{"$regex":"^eans","$options":""}}},{"nation_s.n_regionkey":4}]}}],"cursor":{},"idx":144}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 145
```
{"aggregate":"partsupp","pipeline":[
{"$lookup":{"from":"supplier","localField":"ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$or":[{"s_acctbal":{"$eq":-686.97}},{"s_nationkey":24}]}},
{"$match":{"$or":[{"s_nationkey":23},{"s_acctbal":{"$gt":7627.85}}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$nor":[{"n_regionkey":2}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$lookup":{"from":"region","localField":"nation_s.n_regionkey","foreignField":"r_regionkey","pipeline":[
{"$match":{"$nor":[{"r_regionkey":0}]}}],"as":"region_s"}},
{"$unwind":"$region_s"},
{"$match":{"$or":[{"ps_availqty":{"$gte":6104}},{"supplier.s_acctbal":{"$lt":-334.52}}]}}],"cursor":{},"idx":145}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 146
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$and":[{"ps_comment":{"$regex":{"$regex":"^ am","$options":""}}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"lineitem","localField":"partsupp.ps_partkey","foreignField":"l_partkey","pipeline":[
{"$match":{"$and":[{"l_receiptdate":{"$gte":"1997-03-31T00:00:00.000Z"}},{"l_linenumber":{"$lt":2}}]}}],"as":"lineitem"}},
{"$unwind":"$lineitem"},
{"$match":{"$nor":[{"lineitem.l_extendedprice":{"$gte":1236.32}},{"p_size":{"$gt":44}},{"lineitem.l_shipdate":{"$eq":"1998-06-20T00:00:00.000Z"}},{"lineitem.l_partkey":12202},{"lineitem.l_shipdate":{"$lt":"1997-11-20T00:00:00.000Z"}}]}}],"cursor":{},"idx":146}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 147
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$nor":[{"ps_supplycost":{"$gt":773.39}},{"ps_availqty":{"$eq":9530}}]}},
{"$match":{"$and":[{"ps_comment":{"$regex":{"$regex":"^t","$options":""}}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"supplier","localField":"partsupp.ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$or":[{"s_nationkey":23},{"s_nationkey":19},{"s_acctbal":{"$gte":6404.51}},{"s_acctbal":{"$gte":9583.11}},{"s_name":"Supplier#000000587"}]}},
{"$match":{"$nor":[{"s_nationkey":9}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$or":[{"n_regionkey":2},{"n_name":{"$in":["SAUDI ARABIA","INDONESIA"]}},{"n_regionkey":2}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$match":{"$nor":[{"p_comment":{"$regex":{"$regex":"^u","$options":""}}},{"supplier.s_nationkey":13}]}}],"cursor":{},"idx":147}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 148
```
{"aggregate":"partsupp","pipeline":[
{"$lookup":{"from":"supplier","localField":"ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$nor":[{"s_name":"Supplier#000000137"}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$nor":[{"n_name":"ETHIOPIA"},{"n_name":"JAPAN"},{"n_name":{"$in":["GERMANY","UNITED STATES","KENYA"]}}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$match":{"$and":[{"nation_s.n_regionkey":1},{"ps_comment":{"$regex":{"$regex":"^y","$options":""}}}]}}],"cursor":{},"idx":148}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 149
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$nor":[{"ps_supplycost":{"$gt":705.79}},{"ps_availqty":{"$eq":7854}},{"ps_comment":{"$regex":{"$regex":"^bold","$options":""}}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"supplier","localField":"partsupp.ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$nor":[{"s_name":"Supplier#000000201"},{"s_acctbal":{"$eq":837.27}},{"s_acctbal":{"$lte":7174.74}}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$and":[{"n_name":"INDONESIA"},{"n_regionkey":2}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$lookup":{"from":"region","localField":"nation_s.n_regionkey","foreignField":"r_regionkey","pipeline":[
{"$match":{"$and":[{"r_regionkey":2}]}},
{"$match":{"$or":[{"r_regionkey":3},{"r_name":"AMERICA"},{"r_regionkey":1},{"r_regionkey":2}]}}],"as":"region_s"}},
{"$unwind":"$region_s"},
{"$match":{"$nor":[{"p_partkey":15534},{"nation_s.n_regionkey":4}]}}],"cursor":{},"idx":149}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 150
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$or":[{"ps_comment":{"$regex":{"$regex":"^ bl","$options":""}}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"supplier","localField":"partsupp.ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$or":[{"s_acctbal":{"$gt":7937.31}},{"s_acctbal":{"$lte":6938.43}}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$nor":[{"n_regionkey":1}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$lookup":{"from":"region","localField":"nation_s.n_regionkey","foreignField":"r_regionkey","pipeline":[
{"$match":{"$nor":[{"r_name":"MIDDLE EAST"},{"r_regionkey":1}]}}],"as":"region_s"}},
{"$unwind":"$region_s"},
{"$match":{"$nor":[{"region_s.r_regionkey":4}]}}],"cursor":{},"idx":150}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 151
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$and":[{"ps_supplycost":{"$gt":257.33}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"supplier","localField":"partsupp.ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$and":[{"s_acctbal":{"$gte":1050.66}},{"s_acctbal":{"$gte":1050.66}}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$nor":[{"n_name":{"$in":["CANADA","ARGENTINA","RUSSIA"]}},{"n_name":"JAPAN"}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$match":{"$and":[{"p_type":"PROMO BRUSHED NICKEL"}]}}],"cursor":{},"idx":151}
```
### >>> Subjoin 151-0
```
db.part.aggregate(EJSON.deserialize(
[
{"$match":{"p_type":{"$eq":"PROMO BRUSHED NICKEL"}}}]
));
```
Subjoin plan:
```
COLLSCAN: plan_stability_subjoin_cardinality_md.part {"p_type":{"$eq":"PROMO BRUSHED NICKEL"}}
```
Estimated cardinality: 100  
Actual cardinality: 148  
Orders of magnitude: 0

---
### >>> Subjoin 151-1
```
db.part.aggregate(EJSON.deserialize(
[
{"$match":{"p_type":{"$eq":"PROMO BRUSHED NICKEL"}}},
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","as":"partsupp","pipeline":[
{"$match":{"$and":[{"ps_supplycost":{"$gt":257.33}},{}]}}]}},
{"$unwind":"$partsupp"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$partsupp"]}}}]
));
```
Subjoin plan:
```
INLJ p_partkey = ps_partkey
  -> [none] COLLSCAN: plan_stability_subjoin_cardinality_md.part {"p_type":{"$eq":"PROMO BRUSHED NICKEL"}} 
  -> [partsupp] FETCH: plan_stability_subjoin_cardinality_md.partsupp {"ps_supplycost":{"$gt":257.33}} 
      -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.partsupp ps_partkey_1
```
Estimated cardinality: 301  
Actual cardinality: 431  
Orders of magnitude: 0

---
### >>> Subjoin 151-2
```
db.part.aggregate(EJSON.deserialize(
[
{"$match":{"p_type":{"$eq":"PROMO BRUSHED NICKEL"}}},
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","as":"partsupp","pipeline":[
{"$match":{"$and":[{"ps_supplycost":{"$gt":257.33}},{}]}}]}},
{"$unwind":"$partsupp"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$partsupp"]}}},
{"$lookup":{"from":"supplier","localField":"ps_suppkey","foreignField":"s_suppkey","as":"supplier","pipeline":[
{"$match":{"$and":[{"s_acctbal":{"$gte":1050.66}},{"s_acctbal":{"$gte":1050.66}}]}}]}},
{"$unwind":"$supplier"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$supplier"]}}}]
));
```
Subjoin plan:
```
HJ partsupp.ps_suppkey = s_suppkey
  -> [none] INLJ p_partkey = ps_partkey
      -> [none] COLLSCAN: plan_stability_subjoin_cardinality_md.part {"p_type":{"$eq":"PROMO BRUSHED NICKEL"}} 
      -> [partsupp] FETCH: plan_stability_subjoin_cardinality_md.partsupp {"ps_supplycost":{"$gt":257.33}} 
          -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.partsupp ps_partkey_1
  -> [supplier] COLLSCAN: plan_stability_subjoin_cardinality_md.supplier {"$and":[{"s_acctbal":{"$gte":1050.66}},{"s_acctbal":{"$gte":1050.66}}]}
```
Estimated cardinality: 244  
Actual cardinality: 348  
Orders of magnitude: 0

---
### >>> Subjoin 151-3
```
db.part.aggregate(EJSON.deserialize(
[
{"$match":{"p_type":{"$eq":"PROMO BRUSHED NICKEL"}}},
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","as":"partsupp","pipeline":[
{"$match":{"$and":[{"ps_supplycost":{"$gt":257.33}},{}]}}]}},
{"$unwind":"$partsupp"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$partsupp"]}}},
{"$lookup":{"from":"supplier","localField":"ps_suppkey","foreignField":"s_suppkey","as":"supplier","pipeline":[
{"$match":{"$and":[{"s_acctbal":{"$gte":1050.66}},{"s_acctbal":{"$gte":1050.66}}]}}]}},
{"$unwind":"$supplier"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$supplier"]}}},
{"$lookup":{"from":"nation","localField":"s_nationkey","foreignField":"n_nationkey","as":"nation","pipeline":[
{"$match":{"$nor":[{"n_name":{"$eq":"JAPAN"}},{"n_name":{"$in":["ARGENTINA","CANADA","RUSSIA"]}}]}}]}},
{"$unwind":"$nation"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$nation"]}}}]
));
```
Subjoin plan:
```
HJ n_nationkey = supplier.s_nationkey
  -> [nation_s] COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"$nor":[{"n_name":{"$eq":"JAPAN"}},{"n_name":{"$in":["ARGENTINA","CANADA","RUSSIA"]}}]} 
  -> [none] HJ partsupp.ps_suppkey = s_suppkey
      -> [none] INLJ p_partkey = ps_partkey
          -> [none] COLLSCAN: plan_stability_subjoin_cardinality_md.part {"p_type":{"$eq":"PROMO BRUSHED NICKEL"}} 
          -> [partsupp] FETCH: plan_stability_subjoin_cardinality_md.partsupp {"ps_supplycost":{"$gt":257.33}} 
              -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.partsupp ps_partkey_1
      -> [supplier] COLLSCAN: plan_stability_subjoin_cardinality_md.supplier {"$and":[{"s_acctbal":{"$gte":1050.66}},{"s_acctbal":{"$gte":1050.66}}]}
```
Estimated cardinality: 205  
Actual cardinality: 285  
Orders of magnitude: 0

---
## >>> Command idx 152
```
{"aggregate":"supplier","pipeline":[
{"$lookup":{"from":"nation","localField":"s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$nor":[{"n_regionkey":1}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$lookup":{"from":"region","localField":"nation_s.n_regionkey","foreignField":"r_regionkey","pipeline":[
{"$match":{"$or":[{"r_name":"AMERICA"},{"r_name":"MIDDLE EAST"},{"r_name":"AMERICA"}]}}],"as":"region_s"}},
{"$unwind":"$region_s"},
{"$match":{"$nor":[{"s_name":"Supplier#000000454"}]}}],"cursor":{},"idx":152}
```
### >>> Subjoin 152-0
```
db.region.aggregate(EJSON.deserialize(
[
{"$match":{"r_name":{"$in":["AMERICA","MIDDLE EAST"]}}}]
));
```
Subjoin plan:
```
COLLSCAN: plan_stability_subjoin_cardinality_md.region {"r_name":{"$in":["AMERICA","MIDDLE EAST"]}}
```
Estimated cardinality: 2  
Actual cardinality: 2  
Orders of magnitude: 0

---
### >>> Subjoin 152-1
```
db.region.aggregate(EJSON.deserialize(
[
{"$match":{"r_name":{"$in":["AMERICA","MIDDLE EAST"]}}},
{"$lookup":{"from":"nation","localField":"r_regionkey","foreignField":"n_regionkey","as":"nation","pipeline":[
{"$match":{"n_regionkey":{"$not":{"$eq":1}}}}]}},
{"$unwind":"$nation"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$nation"]}}}]
));
```
Subjoin plan:
```
HJ r_regionkey = n_regionkey
  -> [region_s] COLLSCAN: plan_stability_subjoin_cardinality_md.region {"r_name":{"$in":["AMERICA","MIDDLE EAST"]}} 
  -> [nation_s] COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"n_regionkey":{"$not":{"$eq":1}}}
```
Estimated cardinality: 8  
Actual cardinality: 5  
Orders of magnitude: 0

---
### >>> Subjoin 152-2
```
db.region.aggregate(EJSON.deserialize(
[
{"$match":{"r_name":{"$in":["AMERICA","MIDDLE EAST"]}}},
{"$lookup":{"from":"nation","localField":"r_regionkey","foreignField":"n_regionkey","as":"nation","pipeline":[
{"$match":{"n_regionkey":{"$not":{"$eq":1}}}}]}},
{"$unwind":"$nation"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$nation"]}}},
{"$lookup":{"from":"supplier","localField":"n_nationkey","foreignField":"s_nationkey","as":"supplier","pipeline":[
{"$match":{"s_name":{"$not":{"$eq":"Supplier#000000454"}}}}]}},
{"$unwind":"$supplier"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$supplier"]}}}]
));
```
Subjoin plan:
```
HJ nation_s.n_nationkey = s_nationkey
  -> [none] HJ r_regionkey = n_regionkey
      -> [region_s] COLLSCAN: plan_stability_subjoin_cardinality_md.region {"r_name":{"$in":["AMERICA","MIDDLE EAST"]}} 
      -> [nation_s] COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"n_regionkey":{"$not":{"$eq":1}}} 
  -> [none] COLLSCAN: plan_stability_subjoin_cardinality_md.supplier {"s_name":{"$not":{"$eq":"Supplier#000000454"}}}
```
Estimated cardinality: 320  
Actual cardinality: 198  
Orders of magnitude: 0

---
## >>> Command idx 153
```
{"aggregate":"partsupp","pipeline":[
{"$lookup":{"from":"supplier","localField":"ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$nor":[{"s_acctbal":{"$gte":1209.3}}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$and":[{"n_name":{"$nin":["CANADA","ARGENTINA"]}}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$lookup":{"from":"region","localField":"nation_s.n_regionkey","foreignField":"r_regionkey","pipeline":[
{"$match":{"$and":[{"r_regionkey":3}]}}],"as":"region_s"}},
{"$unwind":"$region_s"},
{"$match":{"$nor":[{"ps_availqty":{"$lte":7969}},{"region_s.r_regionkey":1}]}}],"cursor":{},"idx":153}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 154
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$nor":[{"ps_supplycost":{"$gt":548.54}},{"ps_availqty":{"$eq":724}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"supplier","localField":"partsupp.ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$or":[{"s_name":"Supplier#000000661"}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$nor":[{"n_regionkey":3}]}},
{"$match":{"$nor":[{"n_name":"FRANCE"},{"n_regionkey":2}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$lookup":{"from":"region","localField":"nation_s.n_regionkey","foreignField":"r_regionkey","pipeline":[
{"$match":{"$and":[{"r_name":"MIDDLE EAST"}]}},
{"$match":{"$and":[{"r_regionkey":4}]}}],"as":"region_s"}},
{"$unwind":"$region_s"},
{"$match":{"$nor":[{"supplier.s_name":"Supplier#000000200"},{"p_mfgr":{"$nin":["Manufacturer#2","Manufacturer#2"]}}]}}],"cursor":{},"idx":154}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 155
```
{"aggregate":"partsupp","pipeline":[
{"$lookup":{"from":"supplier","localField":"ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$or":[{"s_acctbal":{"$gt":8724.42}}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$or":[{"n_name":"KENYA"},{"n_name":{"$in":["CANADA","MOZAMBIQUE","FRANCE","MOZAMBIQUE"]}}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$match":{"$and":[{"ps_comment":{"$regex":{"$regex":"^ly s","$options":""}}}]}}],"cursor":{},"idx":155}
```
### >>> Subjoin 155-0
```
db.nation.aggregate(EJSON.deserialize(
[
{"$match":{"$or":[{"n_name":{"$eq":"KENYA"}},{"n_name":{"$in":["CANADA","FRANCE","MOZAMBIQUE"]}}]}}]
));
```
Subjoin plan:
```
COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"$or":[{"n_name":{"$eq":"KENYA"}},{"n_name":{"$in":["CANADA","FRANCE","MOZAMBIQUE"]}}]}
```
Estimated cardinality: 4  
Actual cardinality: 4  
Orders of magnitude: 0

---
### >>> Subjoin 155-1
```
db.nation.aggregate(EJSON.deserialize(
[
{"$match":{"$or":[{"n_name":{"$eq":"KENYA"}},{"n_name":{"$in":["CANADA","FRANCE","MOZAMBIQUE"]}}]}},
{"$lookup":{"from":"supplier","localField":"n_nationkey","foreignField":"s_nationkey","as":"supplier","pipeline":[
{"$match":{"s_acctbal":{"$gt":8724.42}}}]}},
{"$unwind":"$supplier"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$supplier"]}}}]
));
```
Subjoin plan:
```
HJ n_nationkey = s_nationkey
  -> [nation_s] COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"$or":[{"n_name":{"$eq":"KENYA"}},{"n_name":{"$in":["CANADA","FRANCE","MOZAMBIQUE"]}}]} 
  -> [supplier] COLLSCAN: plan_stability_subjoin_cardinality_md.supplier {"s_acctbal":{"$gt":8724.42}}
```
Estimated cardinality: 19  
Actual cardinality: 13  
Orders of magnitude: 0

---
### >>> Subjoin 155-2
```
db.nation.aggregate(EJSON.deserialize(
[
{"$match":{"$or":[{"n_name":{"$eq":"KENYA"}},{"n_name":{"$in":["CANADA","FRANCE","MOZAMBIQUE"]}}]}},
{"$lookup":{"from":"supplier","localField":"n_nationkey","foreignField":"s_nationkey","as":"supplier","pipeline":[
{"$match":{"s_acctbal":{"$gt":8724.42}}}]}},
{"$unwind":"$supplier"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$supplier"]}}},
{"$lookup":{"from":"partsupp","localField":"s_suppkey","foreignField":"ps_suppkey","as":"partsupp","pipeline":[
{"$match":{"$and":[{"ps_comment":{"$regex":"^ly s"}},{}]}}]}},
{"$unwind":"$partsupp"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$partsupp"]}}}]
));
```
Subjoin plan:
```
INLJ supplier.s_suppkey = ps_suppkey
  -> [none] HJ n_nationkey = s_nationkey
      -> [nation_s] COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"$or":[{"n_name":{"$eq":"KENYA"}},{"n_name":{"$in":["CANADA","FRANCE","MOZAMBIQUE"]}}]} 
      -> [supplier] COLLSCAN: plan_stability_subjoin_cardinality_md.supplier {"s_acctbal":{"$gt":8724.42}} 
  -> [none] FETCH: plan_stability_subjoin_cardinality_md.partsupp {"ps_comment":{"$regex":"^ly s"}} 
      -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.partsupp ps_suppkey_1
```
Estimated cardinality: 6  
Actual cardinality: 1  
Orders of magnitude: 0

---
## >>> Command idx 156
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$or":[{"ps_availqty":{"$lte":6504}}]}},
{"$match":{"$and":[{"ps_supplycost":{"$lt":37.64}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"supplier","localField":"partsupp.ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$nor":[{"s_nationkey":6},{"s_nationkey":6},{"s_acctbal":{"$eq":-609.59}}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$or":[{"n_regionkey":1},{"n_regionkey":3},{"n_name":{"$in":["RUSSIA","PERU"]}}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$lookup":{"from":"region","localField":"nation_s.n_regionkey","foreignField":"r_regionkey","pipeline":[
{"$match":{"$nor":[{"r_regionkey":2}]}}],"as":"region_s"}},
{"$unwind":"$region_s"},
{"$match":{"$and":[{"region_s.r_regionkey":3}]}}],"cursor":{},"idx":156}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 157
```
{"aggregate":"partsupp","pipeline":[
{"$lookup":{"from":"supplier","localField":"ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$and":[{"s_nationkey":14}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$nor":[{"n_name":{"$in":["MOZAMBIQUE","JAPAN","BRAZIL","GERMANY"]}}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$match":{"$or":[{"nation_s.n_name":"RUSSIA"},{"ps_comment":{"$regex":{"$regex":"^ons","$options":""}}}]}}],"cursor":{},"idx":157}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 158
```
{"aggregate":"partsupp","pipeline":[
{"$lookup":{"from":"supplier","localField":"ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$nor":[{"s_name":"Supplier#000000879"}]}},
{"$match":{"$nor":[{"s_acctbal":{"$lt":2781.03}},{"s_nationkey":22}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$nor":[{"n_name":"CANADA"}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$match":{"$and":[{"nation_s.n_regionkey":0},{"ps_comment":{"$regex":{"$regex":"^f","$options":""}}}]}}],"cursor":{},"idx":158}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 159
```
{"aggregate":"partsupp","pipeline":[
{"$lookup":{"from":"supplier","localField":"ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$or":[{"s_name":"Supplier#000000766"},{"s_acctbal":{"$lt":1589.13}}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$nor":[{"n_name":"IRAN"},{"n_regionkey":1},{"n_regionkey":0},{"n_name":"CHINA"}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$lookup":{"from":"region","localField":"nation_s.n_regionkey","foreignField":"r_regionkey","pipeline":[
{"$match":{"$or":[{"r_name":"ASIA"}]}}],"as":"region_s"}},
{"$unwind":"$region_s"},
{"$match":{"$nor":[{"ps_availqty":{"$gt":298}},{"nation_s.n_name":{"$in":["JAPAN","FRANCE"]}}]}}],"cursor":{},"idx":159}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 160
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$and":[{"ps_availqty":{"$gt":9209}},{"ps_supplycost":{"$gt":332.21}},{"ps_comment":{"$regex":{"$regex":"^t","$options":""}}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"supplier","localField":"partsupp.ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$nor":[{"s_acctbal":{"$lt":505.92}},{"s_nationkey":22}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$or":[{"n_regionkey":0},{"n_name":"INDONESIA"}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$match":{"$nor":[{"p_size":{"$gt":7}},{"supplier.o_orderstatus":"F"},{"supplier.s_name":"Supplier#000000264"}]}}],"cursor":{},"idx":160}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 161
```
{"aggregate":"partsupp","pipeline":[
{"$lookup":{"from":"supplier","localField":"ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$or":[{"s_acctbal":{"$lte":-942.73}}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$nor":[{"n_regionkey":2},{"n_name":"MOZAMBIQUE"}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$lookup":{"from":"region","localField":"nation_s.n_regionkey","foreignField":"r_regionkey","pipeline":[
{"$match":{"$nor":[{"r_name":"EUROPE"},{"r_name":"AMERICA"},{"r_regionkey":0}]}}],"as":"region_s"}},
{"$unwind":"$region_s"},
{"$match":{"$and":[{"nation_s.n_regionkey":4}]}}],"cursor":{},"idx":161}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 162
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$and":[{"ps_comment":{"$regex":{"$regex":"^ts ","$options":""}}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"supplier","localField":"partsupp.ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$nor":[{"s_name":"Supplier#000000527"}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$nor":[{"n_name":"RUSSIA"}]}},
{"$match":{"$nor":[{"n_name":"EGYPT"}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$lookup":{"from":"region","localField":"nation_s.n_regionkey","foreignField":"r_regionkey","pipeline":[
{"$match":{"$or":[{"r_name":"ASIA"}]}}],"as":"region_s"}},
{"$unwind":"$region_s"},
{"$match":{"$and":[{"nation_s.n_name":"JAPAN"}]}}],"cursor":{},"idx":162}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 163
```
{"aggregate":"partsupp","pipeline":[
{"$lookup":{"from":"supplier","localField":"ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$nor":[{"s_nationkey":7},{"s_acctbal":{"$gt":5119.09}}]}},
{"$match":{"$or":[{"s_nationkey":14},{"s_nationkey":11},{"s_name":"Supplier#000000137"}]}},
{"$match":{"$nor":[{"s_acctbal":{"$gt":8561.72}}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$and":[{"n_regionkey":0},{"n_regionkey":0}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$match":{"$nor":[{"ps_availqty":{"$lte":5124}},{"nation_s.n_name":"ARGENTINA"}]}}],"cursor":{},"idx":163}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 164
```
{"aggregate":"partsupp","pipeline":[
{"$lookup":{"from":"supplier","localField":"ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$or":[{"s_nationkey":3},{"s_name":"Supplier#000000556"}]}},
{"$match":{"$or":[{"s_acctbal":{"$gte":6835.16}}]}},
{"$match":{"$or":[{"s_acctbal":{"$lt":-467.16}},{"s_acctbal":{"$gt":8512.48}},{"s_name":"Supplier#000000055"}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$or":[{"n_regionkey":1},{"n_regionkey":3}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$match":{"$nor":[{"nation_s.n_regionkey":0}]}}],"cursor":{},"idx":164}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 165
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$nor":[{"ps_supplycost":{"$eq":836.01}}]}},
{"$match":{"$nor":[{"ps_comment":{"$regex":{"$regex":"^ly","$options":""}}},{"ps_availqty":{"$gt":8115}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"supplier","localField":"partsupp.ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$nor":[{"s_nationkey":9}]}},
{"$match":{"$and":[{"s_acctbal":{"$gte":7182.24}}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$or":[{"n_name":{"$nin":["ALGERIA","IRAN"]}}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$match":{"$or":[{"p_name":{"$regex":{"$regex":"^bla","$options":""}}},{"partsupp.ps_comment":{"$regex":{"$regex":"^ing","$options":""}}}]}}],"cursor":{},"idx":165}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 166
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$nor":[{"ps_comment":{"$regex":{"$regex":"^si","$options":""}}}]}},
{"$match":{"$or":[{"ps_supplycost":{"$eq":739.85}},{"ps_comment":{"$regex":{"$regex":"^ i","$options":""}}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"lineitem","localField":"partsupp.ps_partkey","foreignField":"l_partkey","pipeline":[
{"$match":{"$nor":[{"l_quantity":{"$gte":47}},{"l_linenumber":{"$gt":3}}]}},
{"$match":{"$and":[{"l_quantity":{"$gt":33}}]}}],"as":"lineitem"}},
{"$unwind":"$lineitem"},
{"$match":{"$and":[{"p_size":{"$gt":22}},{"lineitem.l_shipinstruct":{"$in":["TAKE BACK RETURN","TAKE BACK RETURN","DELIVER IN PERSON"]}},{"p_retailprice":{"$gt":1416.49}}]}}],"cursor":{},"idx":166}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 167
```
{"aggregate":"partsupp","pipeline":[
{"$lookup":{"from":"supplier","localField":"ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$and":[{"s_acctbal":{"$lte":958.07}},{"s_acctbal":{"$lte":-609.59}}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$and":[{"n_name":{"$nin":["MOROCCO","VIETNAM","IRAQ","RUSSIA"]}}]}},
{"$match":{"$nor":[{"n_regionkey":4},{"n_regionkey":1}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$lookup":{"from":"region","localField":"nation_s.n_regionkey","foreignField":"r_regionkey","pipeline":[
{"$match":{"$nor":[{"r_name":"MIDDLE EAST"}]}},
{"$match":{"$or":[{"r_name":"AFRICA"},{"r_regionkey":4},{"r_regionkey":3}]}},
{"$match":{"$nor":[{"r_regionkey":2}]}}],"as":"region_s"}},
{"$unwind":"$region_s"},
{"$match":{"$nor":[{"nation_s.n_regionkey":4},{"ps_comment":{"$regex":{"$regex":"^d","$options":""}}},{"region_s.r_name":"AMERICA"}]}}],"cursor":{},"idx":167}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 168
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$or":[{"ps_availqty":{"$lt":4038}},{"ps_comment":{"$regex":{"$regex":"^ts i","$options":""}}}]}},
{"$match":{"$nor":[{"ps_comment":{"$regex":{"$regex":"^sly","$options":""}}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"supplier","localField":"partsupp.ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$nor":[{"s_acctbal":{"$gt":7307.62}},{"s_acctbal":{"$lt":6741.18}},{"s_nationkey":2}]}},
{"$match":{"$or":[{"s_acctbal":{"$gt":2152.23}},{"s_acctbal":{"$gte":167.56}}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$nor":[{"n_regionkey":2},{"n_name":"KENYA"}]}},
{"$match":{"$or":[{"n_name":{"$nin":["ROMANIA","MOROCCO","ETHIOPIA"]}}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$match":{"$and":[{"p_comment":{"$regex":{"$regex":"^ousl","$options":""}}}]}}],"cursor":{},"idx":168}
```
### >>> Subjoin 168-0
```
db.nation.aggregate(EJSON.deserialize(
[
{"$match":{"$and":[{"n_name":{"$not":{"$in":["ETHIOPIA","MOROCCO","ROMANIA"]}}},{"$nor":[{"n_name":{"$eq":"KENYA"}},{"n_regionkey":{"$eq":2}}]}]}}]
));
```
Subjoin plan:
```
COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"$and":[{"n_name":{"$not":{"$in":["ETHIOPIA","MOROCCO","ROMANIA"]}}},{"$nor":[{"n_name":{"$eq":"KENYA"}},{"n_regionkey":{"$eq":2}}]}]}
```
Estimated cardinality: 16  
Actual cardinality: 16  
Orders of magnitude: 0

---
### >>> Subjoin 168-1
```
db.nation.aggregate(EJSON.deserialize(
[
{"$match":{"$and":[{"n_name":{"$not":{"$in":["ETHIOPIA","MOROCCO","ROMANIA"]}}},{"$nor":[{"n_name":{"$eq":"KENYA"}},{"n_regionkey":{"$eq":2}}]}]}},
{"$lookup":{"from":"supplier","localField":"n_nationkey","foreignField":"s_nationkey","as":"supplier","pipeline":[
{"$match":{"$and":[{"$or":[{"s_acctbal":{"$gt":2152.23}},{"s_acctbal":{"$gte":167.56}}]},{"$nor":[{"s_nationkey":{"$eq":2}},{"s_acctbal":{"$lt":6741.18}},{"s_acctbal":{"$gt":7307.62}}]}]}}]}},
{"$unwind":"$supplier"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$supplier"]}}}]
));
```
Subjoin plan:
```
HJ n_nationkey = s_nationkey
  -> [nation_s] COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"$and":[{"n_name":{"$not":{"$in":["ETHIOPIA","MOROCCO","ROMANIA"]}}},{"$nor":[{"n_name":{"$eq":"KENYA"}},{"n_regionkey":{"$eq":2}}]}]} 
  -> [supplier] COLLSCAN: plan_stability_subjoin_cardinality_md.supplier {"$and":[{"$or":[{"s_acctbal":{"$gt":2152.23}},{"s_acctbal":{"$gte":167.56}}]},{"$nor":[{"s_nationkey":{"$eq":2}},{"s_acctbal":{"$lt":6741.18}},{"s_acctbal":{"$gt":7307.62}}]}]}
```
Estimated cardinality: 29  
Actual cardinality: 24  
Orders of magnitude: 0

---
### >>> Subjoin 168-2
```
db.nation.aggregate(EJSON.deserialize(
[
{"$match":{"$and":[{"n_name":{"$not":{"$in":["ETHIOPIA","MOROCCO","ROMANIA"]}}},{"$nor":[{"n_name":{"$eq":"KENYA"}},{"n_regionkey":{"$eq":2}}]}]}},
{"$lookup":{"from":"supplier","localField":"n_nationkey","foreignField":"s_nationkey","as":"supplier","pipeline":[
{"$match":{"$and":[{"$or":[{"s_acctbal":{"$gt":2152.23}},{"s_acctbal":{"$gte":167.56}}]},{"$nor":[{"s_nationkey":{"$eq":2}},{"s_acctbal":{"$lt":6741.18}},{"s_acctbal":{"$gt":7307.62}}]}]}}]}},
{"$unwind":"$supplier"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$supplier"]}}},
{"$lookup":{"from":"partsupp","localField":"s_suppkey","foreignField":"ps_suppkey","as":"partsupp","pipeline":[
{"$match":{"$and":[{"$and":[{"$or":[{"ps_availqty":{"$lt":4038}},{"ps_comment":{"$regex":"^ts i"}}]},{"ps_comment":{"$not":{"$regex":"^sly"}}}]},{}]}}]}},
{"$unwind":"$partsupp"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$partsupp"]}}}]
));
```
Subjoin plan:
```
INLJ supplier.s_suppkey = ps_suppkey
  -> [none] HJ n_nationkey = s_nationkey
      -> [nation_s] COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"$and":[{"n_name":{"$not":{"$in":["ETHIOPIA","MOROCCO","ROMANIA"]}}},{"$nor":[{"n_name":{"$eq":"KENYA"}},{"n_regionkey":{"$eq":2}}]}]} 
      -> [supplier] COLLSCAN: plan_stability_subjoin_cardinality_md.supplier {"$and":[{"$or":[{"s_acctbal":{"$gt":2152.23}},{"s_acctbal":{"$gte":167.56}}]},{"$nor":[{"s_nationkey":{"$eq":2}},{"s_acctbal":{"$lt":6741.18}},{"s_acctbal":{"$gt":7307.62}}]}]} 
  -> [partsupp] FETCH: plan_stability_subjoin_cardinality_md.partsupp {"$and":[{"$or":[{"ps_availqty":{"$lt":4038}},{"ps_comment":{"$regex":"^ts i"}}]},{"ps_comment":{"$not":{"$regex":"^sly"}}}]} 
      -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.partsupp ps_suppkey_1
```
Estimated cardinality: 942  
Actual cardinality: 768  
Orders of magnitude: 0

---
### >>> Subjoin 168-3
```
db.nation.aggregate(EJSON.deserialize(
[
{"$match":{"$and":[{"n_name":{"$not":{"$in":["ETHIOPIA","MOROCCO","ROMANIA"]}}},{"$nor":[{"n_name":{"$eq":"KENYA"}},{"n_regionkey":{"$eq":2}}]}]}},
{"$lookup":{"from":"supplier","localField":"n_nationkey","foreignField":"s_nationkey","as":"supplier","pipeline":[
{"$match":{"$and":[{"$or":[{"s_acctbal":{"$gt":2152.23}},{"s_acctbal":{"$gte":167.56}}]},{"$nor":[{"s_nationkey":{"$eq":2}},{"s_acctbal":{"$lt":6741.18}},{"s_acctbal":{"$gt":7307.62}}]}]}}]}},
{"$unwind":"$supplier"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$supplier"]}}},
{"$lookup":{"from":"partsupp","localField":"s_suppkey","foreignField":"ps_suppkey","as":"partsupp","pipeline":[
{"$match":{"$and":[{"$and":[{"$or":[{"ps_availqty":{"$lt":4038}},{"ps_comment":{"$regex":"^ts i"}}]},{"ps_comment":{"$not":{"$regex":"^sly"}}}]},{}]}}]}},
{"$unwind":"$partsupp"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$partsupp"]}}},
{"$lookup":{"from":"part","localField":"ps_partkey","foreignField":"p_partkey","as":"part","pipeline":[
{"$match":{"p_comment":{"$regex":"^ousl"}}}]}},
{"$unwind":"$part"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$part"]}}}]
));
```
Subjoin plan:
```
HJ p_partkey = partsupp.ps_partkey
  -> [none] COLLSCAN: plan_stability_subjoin_cardinality_md.part {"p_comment":{"$regex":"^ousl"}} 
  -> [none] INLJ supplier.s_suppkey = ps_suppkey
      -> [none] HJ n_nationkey = s_nationkey
          -> [nation_s] COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"$and":[{"n_name":{"$not":{"$in":["ETHIOPIA","MOROCCO","ROMANIA"]}}},{"$nor":[{"n_name":{"$eq":"KENYA"}},{"n_regionkey":{"$eq":2}}]}]} 
          -> [supplier] COLLSCAN: plan_stability_subjoin_cardinality_md.supplier {"$and":[{"$or":[{"s_acctbal":{"$gt":2152.23}},{"s_acctbal":{"$gte":167.56}}]},{"$nor":[{"s_nationkey":{"$eq":2}},{"s_acctbal":{"$lt":6741.18}},{"s_acctbal":{"$gt":7307.62}}]}]} 
      -> [partsupp] FETCH: plan_stability_subjoin_cardinality_md.partsupp {"$and":[{"$or":[{"ps_availqty":{"$lt":4038}},{"ps_comment":{"$regex":"^ts i"}}]},{"ps_comment":{"$not":{"$regex":"^sly"}}}]} 
          -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.partsupp ps_suppkey_1
```
Estimated cardinality: 3  
Actual cardinality: 4  
Orders of magnitude: 0

---
## >>> Command idx 169
```
{"aggregate":"partsupp","pipeline":[
{"$lookup":{"from":"supplier","localField":"ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$nor":[{"s_nationkey":8}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$nor":[{"n_regionkey":1},{"n_regionkey":2}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$lookup":{"from":"region","localField":"nation_s.n_regionkey","foreignField":"r_regionkey","pipeline":[
{"$match":{"$and":[{"r_regionkey":0}]}}],"as":"region_s"}},
{"$unwind":"$region_s"},
{"$match":{"$and":[{"ps_comment":{"$regex":{"$regex":"^ly ","$options":""}}},{"region_s.r_name":"AFRICA"}]}}],"cursor":{},"idx":169}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 170
```
{"aggregate":"partsupp","pipeline":[
{"$lookup":{"from":"supplier","localField":"ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$or":[{"s_acctbal":{"$gt":5364.99}}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$nor":[{"n_regionkey":4}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$match":{"$and":[{"ps_supplycost":{"$gte":773.39}},{"ps_availqty":{"$gte":9209}},{"nation_s.n_name":"ROMANIA"}]}}],"cursor":{},"idx":170}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 171
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$and":[{"ps_comment":{"$regex":{"$regex":"^d","$options":""}}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"supplier","localField":"partsupp.ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$nor":[{"s_name":"Supplier#000000390"}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$nor":[{"n_name":"IRAN"}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$lookup":{"from":"region","localField":"nation_s.n_regionkey","foreignField":"r_regionkey","pipeline":[
{"$match":{"$nor":[{"r_regionkey":2},{"r_regionkey":2}]}},
{"$match":{"$or":[{"r_name":"EUROPE"}]}}],"as":"region_s"}},
{"$unwind":"$region_s"},
{"$match":{"$nor":[{"p_partkey":14923},{"region_s.r_regionkey":0},{"region_s.r_regionkey":1}]}}],"cursor":{},"idx":171}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 172
```
{"aggregate":"partsupp","pipeline":[
{"$lookup":{"from":"supplier","localField":"ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$nor":[{"s_acctbal":{"$lte":9747.16}},{"s_nationkey":11}]}},
{"$match":{"$or":[{"s_acctbal":{"$gte":10.33}},{"s_name":"Supplier#000000558"}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$and":[{"n_name":{"$nin":["ETHIOPIA","FRANCE","FRANCE"]}}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$match":{"$nor":[{"nation_s.n_name":{"$in":["JORDAN","ETHIOPIA"]}},{"ps_availqty":{"$gte":724}}]}}],"cursor":{},"idx":172}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 173
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$or":[{"ps_availqty":{"$gt":3991}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"supplier","localField":"partsupp.ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$or":[{"s_acctbal":{"$eq":8924.02}},{"s_acctbal":{"$eq":1871.86}},{"s_name":"Supplier#000000537"}]}},
{"$match":{"$nor":[{"s_nationkey":16}]}},
{"$match":{"$and":[{"s_acctbal":{"$lt":7627.85}}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$or":[{"n_name":{"$nin":["INDONESIA","ARGENTINA"]}},{"n_regionkey":4}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$lookup":{"from":"region","localField":"nation_s.n_regionkey","foreignField":"r_regionkey","pipeline":[
{"$match":{"$or":[{"r_regionkey":4}]}}],"as":"region_s"}},
{"$unwind":"$region_s"},
{"$match":{"$or":[{"nation_s.n_name":"ALGERIA"},{"partsupp.ps_comment":{"$regex":{"$regex":"^ a","$options":""}}},{"p_mfgr":"Manufacturer#3"},{"supplier.s_acctbal":{"$eq":5322.35}}]}}],"cursor":{},"idx":173}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 174
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$or":[{"ps_supplycost":{"$gt":904.85}},{"ps_availqty":{"$lte":1439}},{"ps_comment":{"$regex":{"$regex":"^ular","$options":""}}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"supplier","localField":"partsupp.ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$nor":[{"s_name":"Supplier#000000924"},{"s_acctbal":{"$gt":-609.59}}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$nor":[{"n_name":"RUSSIA"},{"n_regionkey":2},{"n_regionkey":3}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$lookup":{"from":"region","localField":"nation_s.n_regionkey","foreignField":"r_regionkey","pipeline":[
{"$match":{"$nor":[{"r_regionkey":3},{"r_regionkey":0},{"r_name":"ASIA"}]}}],"as":"region_s"}},
{"$unwind":"$region_s"},
{"$match":{"$nor":[{"p_type":"LARGE POLISHED NICKEL"},{"region_s.r_regionkey":2},{"nation_s.n_name":"ROMANIA"}]}}],"cursor":{},"idx":174}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 175
```
{"aggregate":"supplier","pipeline":[
{"$lookup":{"from":"nation","localField":"s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$nor":[{"n_name":"IRAQ"}]}},
{"$match":{"$or":[{"n_name":{"$in":["KENYA","JORDAN","INDONESIA"]}},{"n_regionkey":0}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$lookup":{"from":"region","localField":"nation_s.n_regionkey","foreignField":"r_regionkey","pipeline":[
{"$match":{"$or":[{"r_name":"MIDDLE EAST"},{"r_name":"AFRICA"},{"r_regionkey":1},{"r_name":"EUROPE"}]}},
{"$match":{"$or":[{"r_name":"MIDDLE EAST"}]}}],"as":"region_s"}},
{"$unwind":"$region_s"},
{"$match":{"$nor":[{"region_s.r_regionkey":2}]}}],"cursor":{},"idx":175}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 176
```
{"aggregate":"partsupp","pipeline":[
{"$lookup":{"from":"supplier","localField":"ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$nor":[{"s_acctbal":{"$gt":1209.3}}]}},
{"$match":{"$nor":[{"s_name":"Supplier#000000556"}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$nor":[{"n_regionkey":4},{"n_regionkey":3}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$match":{"$and":[{"nation_s.n_name":"JAPAN"},{"nation_s.n_regionkey":2},{"ps_comment":{"$regex":{"$regex":"^ar ","$options":""}}}]}}],"cursor":{},"idx":176}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 177
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$and":[{"ps_supplycost":{"$gte":210.63}},{"ps_comment":{"$regex":{"$regex":"^ pac","$options":""}}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"supplier","localField":"partsupp.ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$nor":[{"s_acctbal":{"$gt":167.56}}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$match":{"$nor":[{"p_mfgr":"Manufacturer#5"},{"p_partkey":14703},{"supplier.o_totalprice":{"$lte":47090.46}}]}}],"cursor":{},"idx":177}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 178
```
{"aggregate":"partsupp","pipeline":[
{"$lookup":{"from":"supplier","localField":"ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$and":[{"s_acctbal":{"$lte":-707.02}}]}},
{"$match":{"$nor":[{"s_acctbal":{"$gt":747.88}},{"s_nationkey":22}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$nor":[{"n_regionkey":1},{"n_name":"JAPAN"}]}},
{"$match":{"$or":[{"n_regionkey":3},{"n_regionkey":0},{"n_regionkey":0},{"n_regionkey":4}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$lookup":{"from":"region","localField":"nation_s.n_regionkey","foreignField":"r_regionkey","pipeline":[
{"$match":{"$nor":[{"r_name":"EUROPE"}]}}],"as":"region_s"}},
{"$unwind":"$region_s"},
{"$match":{"$nor":[{"nation_s.n_regionkey":1},{"nation_s.n_name":{"$nin":["EGYPT","VIETNAM"]}},{"ps_comment":{"$regex":{"$regex":"^fin","$options":""}}}]}}],"cursor":{},"idx":178}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 179
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$or":[{"ps_availqty":{"$lt":3930}},{"ps_supplycost":{"$gt":889.05}}]}},
{"$match":{"$or":[{"ps_comment":{"$regex":{"$regex":"^th","$options":""}}},{"ps_availqty":{"$gte":6415}}]}},
{"$match":{"$nor":[{"ps_supplycost":{"$gt":670.76}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"supplier","localField":"partsupp.ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$nor":[{"s_acctbal":{"$gte":7448.46}}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$or":[{"n_regionkey":0}]}},
{"$match":{"$nor":[{"n_name":"ARGENTINA"},{"n_regionkey":2}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$match":{"$and":[{"p_name":{"$regex":{"$regex":"^bisq","$options":""}}},{"supplier.s_acctbal":{"$gt":1432.69}},{"nation_s.n_name":{"$nin":["IRAN","MOZAMBIQUE"]}}]}}],"cursor":{},"idx":179}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 180
```
{"aggregate":"supplier","pipeline":[
{"$lookup":{"from":"nation","localField":"s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$or":[{"n_name":{"$nin":["IRAQ","JAPAN","VIETNAM","INDONESIA"]}},{"n_name":{"$nin":["EGYPT","JAPAN"]}}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$lookup":{"from":"region","localField":"nation_s.n_regionkey","foreignField":"r_regionkey","pipeline":[
{"$match":{"$or":[{"r_regionkey":2},{"r_regionkey":2},{"r_name":"AFRICA"}]}}],"as":"region_s"}},
{"$unwind":"$region_s"},
{"$match":{"$nor":[{"s_acctbal":{"$gte":-686.97}}]}}],"cursor":{},"idx":180}
```
### >>> Subjoin 180-0
```
db.region.aggregate(EJSON.deserialize(
[
{"$match":{"$or":[{"r_name":{"$eq":"AFRICA"}},{"r_regionkey":{"$in":[2]}}]}}]
));
```
Subjoin plan:
```
COLLSCAN: plan_stability_subjoin_cardinality_md.region {"$or":[{"r_name":{"$eq":"AFRICA"}},{"r_regionkey":{"$in":[2]}}]}
```
Estimated cardinality: 2  
Actual cardinality: 2  
Orders of magnitude: 0

---
### >>> Subjoin 180-1
```
db.region.aggregate(EJSON.deserialize(
[
{"$match":{"$or":[{"r_name":{"$eq":"AFRICA"}},{"r_regionkey":{"$in":[2]}}]}},
{"$lookup":{"from":"nation","localField":"r_regionkey","foreignField":"n_regionkey","as":"nation","pipeline":[
{"$match":{"$or":[{"n_name":{"$not":{"$in":["INDONESIA","IRAQ","JAPAN","VIETNAM"]}}},{"n_name":{"$not":{"$in":["EGYPT","JAPAN"]}}}]}}]}},
{"$unwind":"$nation"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$nation"]}}}]
));
```
Subjoin plan:
```
HJ r_regionkey = n_regionkey
  -> [region_s] COLLSCAN: plan_stability_subjoin_cardinality_md.region {"$or":[{"r_name":{"$eq":"AFRICA"}},{"r_regionkey":{"$in":[2]}}]} 
  -> [nation_s] COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"$or":[{"n_name":{"$not":{"$in":["INDONESIA","IRAQ","JAPAN","VIETNAM"]}}},{"n_name":{"$not":{"$in":["EGYPT","JAPAN"]}}}]}
```
Estimated cardinality: 10  
Actual cardinality: 9  
Orders of magnitude: 1

---
### >>> Subjoin 180-2
```
db.region.aggregate(EJSON.deserialize(
[
{"$match":{"$or":[{"r_name":{"$eq":"AFRICA"}},{"r_regionkey":{"$in":[2]}}]}},
{"$lookup":{"from":"nation","localField":"r_regionkey","foreignField":"n_regionkey","as":"nation","pipeline":[
{"$match":{"$or":[{"n_name":{"$not":{"$in":["INDONESIA","IRAQ","JAPAN","VIETNAM"]}}},{"n_name":{"$not":{"$in":["EGYPT","JAPAN"]}}}]}}]}},
{"$unwind":"$nation"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$nation"]}}},
{"$lookup":{"from":"supplier","localField":"n_nationkey","foreignField":"s_nationkey","as":"supplier","pipeline":[
{"$match":{"s_acctbal":{"$not":{"$gte":-686.97}}}}]}},
{"$unwind":"$supplier"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$supplier"]}}}]
));
```
Subjoin plan:
```
HJ nation_s.n_nationkey = s_nationkey
  -> [none] HJ r_regionkey = n_regionkey
      -> [region_s] COLLSCAN: plan_stability_subjoin_cardinality_md.region {"$or":[{"r_name":{"$eq":"AFRICA"}},{"r_regionkey":{"$in":[2]}}]} 
      -> [nation_s] COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"$or":[{"n_name":{"$not":{"$in":["INDONESIA","IRAQ","JAPAN","VIETNAM"]}}},{"n_name":{"$not":{"$in":["EGYPT","JAPAN"]}}}]} 
  -> [none] COLLSCAN: plan_stability_subjoin_cardinality_md.supplier {"s_acctbal":{"$not":{"$gte":-686.97}}}
```
Estimated cardinality: 10  
Actual cardinality: 9  
Orders of magnitude: 1

---
## >>> Command idx 181
```
{"aggregate":"supplier","pipeline":[
{"$lookup":{"from":"nation","localField":"s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$nor":[{"n_regionkey":1},{"n_name":"CANADA"}]}},
{"$match":{"$nor":[{"n_regionkey":2}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$lookup":{"from":"region","localField":"nation_s.n_regionkey","foreignField":"r_regionkey","pipeline":[
{"$match":{"$nor":[{"r_name":"AFRICA"}]}}],"as":"region_s"}},
{"$unwind":"$region_s"},
{"$match":{"$nor":[{"s_name":"Supplier#000000783"},{"s_acctbal":{"$gt":-685.94}}]}}],"cursor":{},"idx":181}
```
### >>> Subjoin 181-0
```
db.region.aggregate(EJSON.deserialize(
[
{"$match":{"r_name":{"$not":{"$eq":"AFRICA"}}}}]
));
```
Subjoin plan:
```
COLLSCAN: plan_stability_subjoin_cardinality_md.region {"r_name":{"$not":{"$eq":"AFRICA"}}}
```
Estimated cardinality: 4  
Actual cardinality: 4  
Orders of magnitude: 0

---
### >>> Subjoin 181-1
```
db.region.aggregate(EJSON.deserialize(
[
{"$match":{"r_name":{"$not":{"$eq":"AFRICA"}}}},
{"$lookup":{"from":"nation","localField":"r_regionkey","foreignField":"n_regionkey","as":"nation","pipeline":[
{"$match":{"$and":[{"n_regionkey":{"$not":{"$eq":2}}},{"$nor":[{"n_name":{"$eq":"CANADA"}},{"n_regionkey":{"$eq":1}}]}]}}]}},
{"$unwind":"$nation"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$nation"]}}}]
));
```
Subjoin plan:
```
HJ r_regionkey = n_regionkey
  -> [region_s] COLLSCAN: plan_stability_subjoin_cardinality_md.region {"r_name":{"$not":{"$eq":"AFRICA"}}} 
  -> [nation_s] COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"$and":[{"n_regionkey":{"$not":{"$eq":2}}},{"$nor":[{"n_name":{"$eq":"CANADA"}},{"n_regionkey":{"$eq":1}}]}]}
```
Estimated cardinality: 12  
Actual cardinality: 10  
Orders of magnitude: 0

---
### >>> Subjoin 181-2
```
db.region.aggregate(EJSON.deserialize(
[
{"$match":{"r_name":{"$not":{"$eq":"AFRICA"}}}},
{"$lookup":{"from":"nation","localField":"r_regionkey","foreignField":"n_regionkey","as":"nation","pipeline":[
{"$match":{"$and":[{"n_regionkey":{"$not":{"$eq":2}}},{"$nor":[{"n_name":{"$eq":"CANADA"}},{"n_regionkey":{"$eq":1}}]}]}}]}},
{"$unwind":"$nation"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$nation"]}}},
{"$lookup":{"from":"supplier","localField":"n_nationkey","foreignField":"s_nationkey","as":"supplier","pipeline":[
{"$match":{"$nor":[{"s_name":{"$eq":"Supplier#000000783"}},{"s_acctbal":{"$gt":-685.94}}]}}]}},
{"$unwind":"$supplier"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$supplier"]}}}]
));
```
Subjoin plan:
```
HJ nation_s.n_nationkey = s_nationkey
  -> [none] HJ r_regionkey = n_regionkey
      -> [region_s] COLLSCAN: plan_stability_subjoin_cardinality_md.region {"r_name":{"$not":{"$eq":"AFRICA"}}} 
      -> [nation_s] COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"$and":[{"n_regionkey":{"$not":{"$eq":2}}},{"$nor":[{"n_name":{"$eq":"CANADA"}},{"n_regionkey":{"$eq":1}}]}]} 
  -> [none] COLLSCAN: plan_stability_subjoin_cardinality_md.supplier {"$nor":[{"s_name":{"$eq":"Supplier#000000783"}},{"s_acctbal":{"$gt":-685.94}}]}
```
Estimated cardinality: 14  
Actual cardinality: 12  
Orders of magnitude: 0

---
## >>> Command idx 182
```
{"aggregate":"supplier","pipeline":[
{"$lookup":{"from":"nation","localField":"s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$nor":[{"n_name":{"$nin":["INDONESIA","FRANCE","IRAQ"]}}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$lookup":{"from":"region","localField":"nation_s.n_regionkey","foreignField":"r_regionkey","pipeline":[
{"$match":{"$or":[{"r_regionkey":3},{"r_name":"MIDDLE EAST"},{"r_name":"MIDDLE EAST"}]}}],"as":"region_s"}},
{"$unwind":"$region_s"},
{"$match":{"$nor":[{"s_name":"Supplier#000000290"},{"region_s.r_name":"AFRICA"},{"s_acctbal":{"$gt":7619.85}},{"region_s.r_name":"MIDDLE EAST"}]}}],"cursor":{},"idx":182}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 183
```
{"aggregate":"partsupp","pipeline":[
{"$lookup":{"from":"supplier","localField":"ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$or":[{"s_nationkey":2},{"s_acctbal":{"$lte":8561.72}},{"s_name":"Supplier#000000103"}]}},
{"$match":{"$nor":[{"s_acctbal":{"$gt":4327.86}},{"s_name":"Supplier#000000928"},{"s_acctbal":{"$gte":-609.59}}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$nor":[{"n_name":{"$in":["FRANCE","KENYA"]}}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$match":{"$or":[{"nation_s.n_regionkey":2}]}}],"cursor":{},"idx":183}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 184
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$or":[{"ps_availqty":{"$lt":9806}}]}},
{"$match":{"$and":[{"ps_supplycost":{"$gt":164.22}}]}},
{"$match":{"$nor":[{"ps_comment":{"$regex":{"$regex":"^fil","$options":""}}},{"ps_comment":{"$regex":{"$regex":"^s","$options":""}}},{"ps_availqty":{"$gte":5275}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"supplier","localField":"partsupp.ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$and":[{"s_acctbal":{"$lte":3580.35}}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$and":[{"n_name":{"$in":["ROMANIA","RUSSIA"]}}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$lookup":{"from":"region","localField":"nation_s.n_regionkey","foreignField":"r_regionkey","pipeline":[
{"$match":{"$and":[{"r_name":"EUROPE"},{"r_name":"EUROPE"}]}}],"as":"region_s"}},
{"$unwind":"$region_s"},
{"$match":{"$and":[{"p_name":{"$regex":{"$regex":"^h","$options":""}}},{"region_s.r_regionkey":3}]}}],"cursor":{},"idx":184}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 185
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$and":[{"ps_comment":{"$regex":{"$regex":"^t","$options":""}}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"lineitem","localField":"partsupp.ps_partkey","foreignField":"l_partkey","pipeline":[
{"$match":{"$or":[{"l_suppkey":162},{"l_partkey":13647}]}}],"as":"lineitem"}},
{"$unwind":"$lineitem"},
{"$match":{"$nor":[{"lineitem.l_returnflag":{"$in":["A","A"]}}]}}],"cursor":{},"idx":185}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 186
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$nor":[{"ps_availqty":{"$gte":9410}},{"ps_availqty":{"$eq":1972}}]}},
{"$match":{"$nor":[{"ps_comment":{"$regex":{"$regex":"^ si","$options":""}}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"supplier","localField":"partsupp.ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$nor":[{"s_name":"Supplier#000000904"}]}},
{"$match":{"$nor":[{"s_name":"Supplier#000000162"}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$nor":[{"n_regionkey":2},{"n_regionkey":1}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$match":{"$and":[{"p_comment":{"$regex":{"$regex":"^rio","$options":""}}},{"nation_s.n_regionkey":4}]}}],"cursor":{},"idx":186}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 187
```
{"aggregate":"supplier","pipeline":[
{"$lookup":{"from":"nation","localField":"s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$nor":[{"n_regionkey":4},{"n_regionkey":0}]}},
{"$match":{"$or":[{"n_name":{"$in":["CANADA","ETHIOPIA","JAPAN"]}}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$lookup":{"from":"region","localField":"nation_s.n_regionkey","foreignField":"r_regionkey","pipeline":[
{"$match":{"$nor":[{"r_regionkey":4},{"r_regionkey":3}]}}],"as":"region_s"}},
{"$unwind":"$region_s"},
{"$match":{"$nor":[{"s_name":"Supplier#000000920"},{"s_acctbal":{"$gt":3222.71}},{"region_s.r_regionkey":2}]}}],"cursor":{},"idx":187}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 188
```
{"aggregate":"supplier","pipeline":[
{"$lookup":{"from":"nation","localField":"s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$or":[{"n_name":{"$in":["SAUDI ARABIA","JAPAN"]}}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$lookup":{"from":"region","localField":"nation_s.n_regionkey","foreignField":"r_regionkey","pipeline":[
{"$match":{"$or":[{"r_regionkey":2},{"r_regionkey":4}]}}],"as":"region_s"}},
{"$unwind":"$region_s"},
{"$match":{"$nor":[{"s_nationkey":15},{"region_s.r_name":"EUROPE"},{"s_acctbal":{"$gte":-609.59}}]}}],"cursor":{},"idx":188}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 189
```
{"aggregate":"supplier","pipeline":[
{"$lookup":{"from":"nation","localField":"s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$or":[{"n_name":{"$in":["KENYA","JORDAN","INDIA"]}}]}},
{"$match":{"$nor":[{"n_regionkey":1},{"n_regionkey":4}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$lookup":{"from":"region","localField":"nation_s.n_regionkey","foreignField":"r_regionkey","pipeline":[
{"$match":{"$nor":[{"r_name":"AFRICA"},{"r_name":"AFRICA"}]}}],"as":"region_s"}},
{"$unwind":"$region_s"},
{"$match":{"$nor":[{"s_name":"Supplier#000000517"},{"region_s.r_name":"MIDDLE EAST"},{"s_acctbal":{"$gte":10.33}}]}}],"cursor":{},"idx":189}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 190
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$or":[{"ps_supplycost":{"$gte":636.17}},{"ps_supplycost":{"$lt":780.26}},{"ps_comment":{"$regex":{"$regex":"^ f","$options":""}}},{"ps_comment":{"$regex":{"$regex":"^hin","$options":""}}}]}},
{"$match":{"$or":[{"ps_supplycost":{"$lt":977.14}},{"ps_comment":{"$regex":{"$regex":"^s ","$options":""}}}]}},
{"$match":{"$nor":[{"ps_supplycost":{"$eq":184.87}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"supplier","localField":"partsupp.ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$or":[{"s_acctbal":{"$lte":8436.92}}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$and":[{"n_name":"CHINA"}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$match":{"$and":[{"nation_s.n_name":{"$nin":["EGYPT","MOROCCO","UNITED KINGDOM"]}},{"p_brand":"Brand#22"}]}}],"cursor":{},"idx":190}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 191
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$nor":[{"ps_availqty":{"$lt":3991}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"supplier","localField":"partsupp.ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$nor":[{"s_name":"Supplier#000000478"},{"s_nationkey":18}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$match":{"$and":[{"p_comment":{"$regex":{"$regex":"^usu","$options":""}}},{"supplier.s_acctbal":{"$gte":7337.45}}]}}],"cursor":{},"idx":191}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 192
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$and":[{"ps_comment":{"$regex":{"$regex":"^f","$options":""}}},{"ps_supplycost":{"$gt":587.19}}]}},
{"$match":{"$or":[{"ps_availqty":{"$lt":8052}},{"ps_availqty":{"$eq":9578}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"lineitem","localField":"partsupp.ps_partkey","foreignField":"l_partkey","pipeline":[
{"$match":{"$and":[{"l_shipinstruct":"TAKE BACK RETURN"}]}},
{"$match":{"$or":[{"l_shipmode":{"$in":["AIR","MAIL","FOB","SHIP"]}}]}}],"as":"lineitem"}},
{"$unwind":"$lineitem"},
{"$match":{"$and":[{"p_comment":{"$regex":{"$regex":"^lit","$options":""}}},{"lineitem.l_shipdate":{"$gte":"1997-06-13T00:00:00.000Z"}}]}}],"cursor":{},"idx":192}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 193
```
{"aggregate":"partsupp","pipeline":[
{"$lookup":{"from":"supplier","localField":"ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$or":[{"s_acctbal":{"$lt":-170.22}},{"s_name":"Supplier#000000467"},{"s_name":"Supplier#000000783"}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$or":[{"n_name":{"$nin":["BRAZIL","IRAN"]}}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$lookup":{"from":"region","localField":"nation_s.n_regionkey","foreignField":"r_regionkey","pipeline":[
{"$match":{"$and":[{"r_name":"MIDDLE EAST"}]}},
{"$match":{"$or":[{"r_name":"MIDDLE EAST"},{"r_name":"AMERICA"}]}}],"as":"region_s"}},
{"$unwind":"$region_s"},
{"$match":{"$and":[{"ps_comment":{"$regex":{"$regex":"^d","$options":""}}},{"region_s.r_name":"MIDDLE EAST"}]}}],"cursor":{},"idx":193}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 194
```
{"aggregate":"supplier","pipeline":[
{"$lookup":{"from":"nation","localField":"s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$or":[{"n_regionkey":4},{"n_name":{"$nin":["JAPAN","MOROCCO","IRAQ"]}}]}},
{"$match":{"$nor":[{"n_name":"MOZAMBIQUE"},{"n_regionkey":1}]}},
{"$match":{"$nor":[{"n_name":"GERMANY"},{"n_regionkey":2}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$lookup":{"from":"region","localField":"nation_s.n_regionkey","foreignField":"r_regionkey","pipeline":[
{"$match":{"$nor":[{"r_regionkey":4}]}}],"as":"region_s"}},
{"$unwind":"$region_s"},
{"$match":{"$nor":[{"s_name":"Supplier#000000788"}]}}],"cursor":{},"idx":194}
```
### >>> Subjoin 194-0
```
db.region.aggregate(EJSON.deserialize(
[
{"$match":{"r_regionkey":{"$not":{"$eq":4}}}}]
));
```
Subjoin plan:
```
COLLSCAN: plan_stability_subjoin_cardinality_md.region {"r_regionkey":{"$not":{"$eq":4}}}
```
Estimated cardinality: 4  
Actual cardinality: 4  
Orders of magnitude: 0

---
### >>> Subjoin 194-1
```
db.region.aggregate(EJSON.deserialize(
[
{"$match":{"r_regionkey":{"$not":{"$eq":4}}}},
{"$lookup":{"from":"nation","localField":"r_regionkey","foreignField":"n_regionkey","as":"nation","pipeline":[
{"$match":{"$and":[{"$or":[{"n_regionkey":{"$eq":4}},{"n_name":{"$not":{"$in":["IRAQ","JAPAN","MOROCCO"]}}}]},{"$nor":[{"n_name":{"$eq":"MOZAMBIQUE"}},{"n_regionkey":{"$eq":1}}]},{"$nor":[{"n_name":{"$eq":"GERMANY"}},{"n_regionkey":{"$eq":2}}]}]}}]}},
{"$unwind":"$nation"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$nation"]}}}]
));
```
Subjoin plan:
```
HJ r_regionkey = n_regionkey
  -> [region_s] COLLSCAN: plan_stability_subjoin_cardinality_md.region {"r_regionkey":{"$not":{"$eq":4}}} 
  -> [nation_s] COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"$and":[{"$or":[{"n_regionkey":{"$eq":4}},{"n_name":{"$not":{"$in":["IRAQ","JAPAN","MOROCCO"]}}}]},{"$nor":[{"n_name":{"$eq":"MOZAMBIQUE"}},{"n_regionkey":{"$eq":1}}]},{"$nor":[{"n_name":{"$eq":"GERMANY"}},{"n_regionkey":{"$eq":2}}]}]}
```
Estimated cardinality: 10  
Actual cardinality: 7  
Orders of magnitude: 1

---
### >>> Subjoin 194-2
```
db.region.aggregate(EJSON.deserialize(
[
{"$match":{"r_regionkey":{"$not":{"$eq":4}}}},
{"$lookup":{"from":"nation","localField":"r_regionkey","foreignField":"n_regionkey","as":"nation","pipeline":[
{"$match":{"$and":[{"$or":[{"n_regionkey":{"$eq":4}},{"n_name":{"$not":{"$in":["IRAQ","JAPAN","MOROCCO"]}}}]},{"$nor":[{"n_name":{"$eq":"MOZAMBIQUE"}},{"n_regionkey":{"$eq":1}}]},{"$nor":[{"n_name":{"$eq":"GERMANY"}},{"n_regionkey":{"$eq":2}}]}]}}]}},
{"$unwind":"$nation"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$nation"]}}},
{"$lookup":{"from":"supplier","localField":"n_nationkey","foreignField":"s_nationkey","as":"supplier","pipeline":[
{"$match":{"s_name":{"$not":{"$eq":"Supplier#000000788"}}}}]}},
{"$unwind":"$supplier"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$supplier"]}}}]
));
```
Subjoin plan:
```
HJ nation_s.n_nationkey = s_nationkey
  -> [none] HJ r_regionkey = n_regionkey
      -> [region_s] COLLSCAN: plan_stability_subjoin_cardinality_md.region {"r_regionkey":{"$not":{"$eq":4}}} 
      -> [nation_s] COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"$and":[{"$or":[{"n_regionkey":{"$eq":4}},{"n_name":{"$not":{"$in":["IRAQ","JAPAN","MOROCCO"]}}}]},{"$nor":[{"n_name":{"$eq":"MOZAMBIQUE"}},{"n_regionkey":{"$eq":1}}]},{"$nor":[{"n_name":{"$eq":"GERMANY"}},{"n_regionkey":{"$eq":2}}]}]} 
  -> [none] COLLSCAN: plan_stability_subjoin_cardinality_md.supplier {"s_name":{"$not":{"$eq":"Supplier#000000788"}}}
```
Estimated cardinality: 384  
Actual cardinality: 259  
Orders of magnitude: 0

---
## >>> Command idx 195
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$or":[{"ps_comment":{"$regex":{"$regex":"^i","$options":""}}},{"ps_availqty":{"$lte":7214}},{"ps_comment":{"$regex":{"$regex":"^ iro","$options":""}}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"lineitem","localField":"partsupp.ps_partkey","foreignField":"l_partkey","pipeline":[
{"$match":{"$or":[{"l_commitdate":{"$lte":"1996-11-20T00:00:00.000Z"}}]}}],"as":"lineitem"}},
{"$unwind":"$lineitem"},
{"$match":{"$and":[{"lineitem.l_quantity":{"$eq":2}},{"p_brand":"Brand#31"},{"lineitem.l_shipmode":"REG AIR"}]}}],"cursor":{},"idx":195}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 196
```
{"aggregate":"partsupp","pipeline":[
{"$lookup":{"from":"supplier","localField":"ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$nor":[{"s_name":"Supplier#000000783"}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$nor":[{"n_name":"VIETNAM"},{"n_regionkey":0},{"n_regionkey":4}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$lookup":{"from":"region","localField":"nation_s.n_regionkey","foreignField":"r_regionkey","pipeline":[
{"$match":{"$nor":[{"r_regionkey":0}]}}],"as":"region_s"}},
{"$unwind":"$region_s"},
{"$match":{"$and":[{"ps_comment":{"$regex":{"$regex":"^ bli","$options":""}}},{"region_s.r_name":"ASIA"},{"nation_s.n_regionkey":2}]}}],"cursor":{},"idx":196}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 197
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$nor":[{"ps_supplycost":{"$gte":587.19}}]}},
{"$match":{"$nor":[{"ps_comment":{"$regex":{"$regex":"^ula","$options":""}}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"supplier","localField":"partsupp.ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$nor":[{"s_name":"Supplier#000000921"}]}},
{"$match":{"$or":[{"s_acctbal":{"$eq":7844.41}},{"s_acctbal":{"$gte":8210.13}}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$or":[{"n_regionkey":4},{"n_regionkey":3}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$lookup":{"from":"region","localField":"nation_s.n_regionkey","foreignField":"r_regionkey","pipeline":[
{"$match":{"$and":[{"r_regionkey":3}]}}],"as":"region_s"}},
{"$unwind":"$region_s"},
{"$match":{"$and":[{"p_name":{"$regex":{"$regex":"^li","$options":""}}},{"nation_s.n_name":{"$nin":["SAUDI ARABIA","GERMANY","KENYA"]}}]}}],"cursor":{},"idx":197}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 198
```
{"aggregate":"partsupp","pipeline":[
{"$lookup":{"from":"supplier","localField":"ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$nor":[{"s_acctbal":{"$lt":5119.09}}]}},
{"$match":{"$or":[{"s_acctbal":{"$gt":1333.75}},{"s_name":"Supplier#000000676"}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$nor":[{"n_name":"IRAQ"}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$lookup":{"from":"region","localField":"nation_s.n_regionkey","foreignField":"r_regionkey","pipeline":[
{"$match":{"$or":[{"r_name":"AMERICA"},{"r_regionkey":4}]}}],"as":"region_s"}},
{"$unwind":"$region_s"},
{"$match":{"$and":[{"ps_supplycost":{"$gt":247.77}},{"ps_comment":{"$regex":{"$regex":"^gs","$options":""}}},{"nation_s.n_regionkey":1}]}}],"cursor":{},"idx":198}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 199
```
{"aggregate":"partsupp","pipeline":[
{"$lookup":{"from":"supplier","localField":"ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$and":[{"s_acctbal":{"$lt":5364.99}}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$nor":[{"n_regionkey":3}]}},
{"$match":{"$or":[{"n_name":"ALGERIA"},{"n_regionkey":0}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$match":{"$and":[{"nation_s.n_name":{"$in":["ETHIOPIA","INDONESIA","EGYPT"]}},{"nation_s.n_regionkey":0},{"ps_comment":{"$regex":{"$regex":"^hi","$options":""}}}]}}],"cursor":{},"idx":199}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 200
```
{"aggregate":"partsupp","pipeline":[
{"$lookup":{"from":"supplier","localField":"ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$or":[{"s_name":"Supplier#000000151"},{"s_acctbal":{"$gt":7241.4}}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$or":[{"n_regionkey":2}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$match":{"$and":[{"nation_s.n_name":{"$nin":["MOZAMBIQUE","JAPAN"]}},{"ps_comment":{"$regex":{"$regex":"^th","$options":""}}},{"ps_availqty":{"$gte":9622}}]}}],"cursor":{},"idx":200}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 202
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$nor":[{"ps_comment":{"$regex":{"$regex":"^b","$options":""}}},{"ps_comment":{"$regex":{"$regex":"^hin","$options":""}}}]}},
{"$match":{"$nor":[{"ps_comment":{"$regex":{"$regex":"^ep","$options":""}}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"supplier","localField":"partsupp.ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$nor":[{"s_acctbal":{"$lt":7937.31}}]}},
{"$match":{"$or":[{"s_nationkey":17},{"s_nationkey":12}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$nor":[{"n_name":"RUSSIA"}]}},
{"$match":{"$nor":[{"n_regionkey":3}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$match":{"$or":[{"supplier.s_acctbal":{"$gt":9681.99}},{"p_container":{"$in":["LG CAN","SM BAG"]}}]}}],"cursor":{},"idx":202}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 205
```
{"aggregate":"supplier","pipeline":[
{"$lookup":{"from":"nation","localField":"s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$and":[{"n_name":{"$nin":["INDIA","CHINA","ETHIOPIA"]}}]}},
{"$match":{"$or":[{"n_name":"ARGENTINA"},{"n_name":"MOROCCO"}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$lookup":{"from":"region","localField":"nation_s.n_regionkey","foreignField":"r_regionkey","pipeline":[
{"$match":{"$nor":[{"r_name":"EUROPE"}]}},
{"$match":{"$nor":[{"r_name":"MIDDLE EAST"},{"r_name":"AFRICA"}]}}],"as":"region_s"}},
{"$unwind":"$region_s"},
{"$match":{"$or":[{"region_s.r_regionkey":1},{"region_s.r_name":"ASIA"},{"nation_s.n_regionkey":3},{"nation_s.n_regionkey":3}]}}],"cursor":{},"idx":205}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 206
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$nor":[{"ps_availqty":{"$gte":9400}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"lineitem","localField":"partsupp.ps_partkey","foreignField":"l_partkey","pipeline":[
{"$match":{"$and":[{"l_quantity":{"$lt":27}}]}}],"as":"lineitem"}},
{"$unwind":"$lineitem"},
{"$match":{"$or":[{"p_name":"sienna misty thistle medium powder"},{"lineitem.l_quantity":{"$eq":28}}]}}],"cursor":{},"idx":206}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 207
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$or":[{"ps_availqty":{"$lte":6013}},{"ps_comment":{"$regex":{"$regex":"^y ev","$options":""}}}]}},
{"$match":{"$nor":[{"ps_availqty":{"$lt":756}}]}},
{"$match":{"$nor":[{"ps_comment":{"$regex":{"$regex":"^fu","$options":""}}},{"ps_supplycost":{"$eq":498.13}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"lineitem","localField":"partsupp.ps_partkey","foreignField":"l_partkey","pipeline":[
{"$match":{"$or":[{"l_tax":{"$gt":0.06}}]}}],"as":"lineitem"}},
{"$unwind":"$lineitem"},
{"$match":{"$or":[{"lineitem.l_tax":{"$gt":0.08}},{"p_comment":{"$regex":{"$regex":"^wa","$options":""}}}]}}],"cursor":{},"idx":207}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 208
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$and":[{"ps_comment":{"$regex":{"$regex":"^s a","$options":""}}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"supplier","localField":"partsupp.ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$or":[{"s_nationkey":9},{"s_nationkey":23},{"s_nationkey":13}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$match":{"$or":[{"p_size":{"$lt":24}},{"p_size":{"$gt":42}}]}}],"cursor":{},"idx":208}
```
### >>> Subjoin 208-0
```
db.supplier.aggregate(EJSON.deserialize(
[
{"$match":{"s_nationkey":{"$in":[9,13,23]}}}]
));
```
Subjoin plan:
```
FETCH: plan_stability_subjoin_cardinality_md.supplier 
  -> IXSCAN: plan_stability_subjoin_cardinality_md.supplier s_nationkey_1 {"s_nationkey":["[9.0, 9.0]","[13.0, 13.0]","[23.0, 23.0]"]}
```
Estimated cardinality: 112  
Actual cardinality: 112  
Orders of magnitude: 0

---
### >>> Subjoin 208-1
```
db.supplier.aggregate(EJSON.deserialize(
[
{"$match":{"s_nationkey":{"$in":[9,13,23]}}},
{"$lookup":{"from":"partsupp","localField":"s_suppkey","foreignField":"ps_suppkey","as":"partsupp","pipeline":[
{"$match":{"$and":[{"ps_comment":{"$regex":"^s a"}},{}]}}]}},
{"$unwind":"$partsupp"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$partsupp"]}}}]
));
```
Subjoin plan:
```
INLJ s_suppkey = ps_suppkey
  -> [supplier] FETCH: plan_stability_subjoin_cardinality_md.supplier 
      -> IXSCAN: plan_stability_subjoin_cardinality_md.supplier s_nationkey_1 {"s_nationkey":["[9.0, 9.0]","[13.0, 13.0]","[23.0, 23.0]"]}
  -> [partsupp] FETCH: plan_stability_subjoin_cardinality_md.partsupp {"ps_comment":{"$regex":"^s a"}} 
      -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.partsupp ps_suppkey_1
```
Estimated cardinality: 108  
Actual cardinality: 47  
Orders of magnitude: 1

---
### >>> Subjoin 208-2
```
db.supplier.aggregate(EJSON.deserialize(
[
{"$match":{"s_nationkey":{"$in":[9,13,23]}}},
{"$lookup":{"from":"partsupp","localField":"s_suppkey","foreignField":"ps_suppkey","as":"partsupp","pipeline":[
{"$match":{"$and":[{"ps_comment":{"$regex":"^s a"}},{}]}}]}},
{"$unwind":"$partsupp"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$partsupp"]}}},
{"$lookup":{"from":"part","localField":"ps_partkey","foreignField":"p_partkey","as":"part","pipeline":[
{"$match":{"$or":[{"p_size":{"$lt":24}},{"p_size":{"$gt":42}}]}}]}},
{"$unwind":"$part"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$part"]}}}]
));
```
Subjoin plan:
```
HJ partsupp.ps_partkey = p_partkey
  -> [none] INLJ s_suppkey = ps_suppkey
      -> [supplier] FETCH: plan_stability_subjoin_cardinality_md.supplier 
          -> IXSCAN: plan_stability_subjoin_cardinality_md.supplier s_nationkey_1 {"s_nationkey":["[9.0, 9.0]","[13.0, 13.0]","[23.0, 23.0]"]}
      -> [partsupp] FETCH: plan_stability_subjoin_cardinality_md.partsupp {"ps_comment":{"$regex":"^s a"}} 
          -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.partsupp ps_suppkey_1
  -> [none] COLLSCAN: plan_stability_subjoin_cardinality_md.part {"$or":[{"p_size":{"$lt":24}},{"p_size":{"$gt":42}}]}
```
Estimated cardinality: 67  
Actual cardinality: 24  
Orders of magnitude: 0

---
## >>> Command idx 209
```
{"aggregate":"lineitem","pipeline":[
{"$lookup":{"from":"orders","localField":"l_orderkey","foreignField":"o_orderkey","pipeline":[
{"$match":{"$nor":[{"o_orderdate":{"$gte":"1992-10-01T00:00:00.000Z"}}]}},
{"$match":{"$and":[{"o_totalprice":{"$lte":202028.47}},{"o_clerk":"Clerk#000000567"}]}}],"as":"orders"}},
{"$unwind":"$orders"},
{"$lookup":{"from":"customer","localField":"orders.o_custkey","foreignField":"c_custkey","pipeline":[
{"$match":{"$nor":[{"c_name":"Customer#000013237"}]}}],"as":"customer"}},
{"$unwind":"$customer"},
{"$lookup":{"from":"supplier","localField":"customer.c_nationkey","foreignField":"s_nationkey","pipeline":[
{"$match":{"$nor":[{"s_acctbal":{"$lte":7901.42}},{"s_name":"Supplier#000000151"},{"s_acctbal":{"$lt":8724.42}}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$match":{"$or":[{"orders.o_shippriority":{"$gt":0}},{"orders.o_totalprice":{"$lte":280828.89}},{"l_linestatus":{"$in":["O"]}},{"orders.o_totalprice":{"$eq":240982.46}},{"customer.c_mktsegment":"HOUSEHOLD"}]}}],"cursor":{},"idx":209}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 210
```
{"aggregate":"supplier","pipeline":[
{"$lookup":{"from":"nation","localField":"s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$nor":[{"n_regionkey":3},{"n_name":"ETHIOPIA"},{"n_regionkey":3}]}},
{"$match":{"$or":[{"n_name":{"$in":["UNITED KINGDOM","CANADA","JAPAN"]}}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$lookup":{"from":"region","localField":"nation_s.n_regionkey","foreignField":"r_regionkey","pipeline":[
{"$match":{"$or":[{"r_name":"ASIA"},{"r_regionkey":3}]}}],"as":"region_s"}},
{"$unwind":"$region_s"},
{"$match":{"$or":[{"s_acctbal":{"$lte":958.07}}]}}],"cursor":{},"idx":210}
```
### >>> Subjoin 210-0
```
db.nation.aggregate(EJSON.deserialize(
[
{"$match":{"$and":[{"n_name":{"$in":["CANADA","JAPAN","UNITED KINGDOM"]}},{"$nor":[{"n_name":{"$eq":"ETHIOPIA"}},{"n_regionkey":{"$eq":3}},{"n_regionkey":{"$eq":3}}]}]}}]
));
```
Subjoin plan:
```
COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"$and":[{"n_name":{"$in":["CANADA","JAPAN","UNITED KINGDOM"]}},{"$nor":[{"n_name":{"$eq":"ETHIOPIA"}},{"n_regionkey":{"$eq":3}},{"n_regionkey":{"$eq":3}}]}]}
```
Estimated cardinality: 2  
Actual cardinality: 2  
Orders of magnitude: 0

---
### >>> Subjoin 210-1
```
db.nation.aggregate(EJSON.deserialize(
[
{"$match":{"$and":[{"n_name":{"$in":["CANADA","JAPAN","UNITED KINGDOM"]}},{"$nor":[{"n_name":{"$eq":"ETHIOPIA"}},{"n_regionkey":{"$eq":3}},{"n_regionkey":{"$eq":3}}]}]}},
{"$lookup":{"from":"region","localField":"n_regionkey","foreignField":"r_regionkey","as":"region","pipeline":[
{"$match":{"$or":[{"r_name":{"$eq":"ASIA"}},{"r_regionkey":{"$eq":3}}]}}]}},
{"$unwind":"$region"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$region"]}}}]
));
```
Subjoin plan:
```
HJ n_regionkey = r_regionkey
  -> [nation_s] COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"$and":[{"n_name":{"$in":["CANADA","JAPAN","UNITED KINGDOM"]}},{"$nor":[{"n_name":{"$eq":"ETHIOPIA"}},{"n_regionkey":{"$eq":3}},{"n_regionkey":{"$eq":3}}]}]} 
  -> [region_s] COLLSCAN: plan_stability_subjoin_cardinality_md.region {"$or":[{"r_name":{"$eq":"ASIA"}},{"r_regionkey":{"$eq":3}}]}
```
Estimated cardinality: 1  
Actual cardinality: 1  
Orders of magnitude: 0

---
### >>> Subjoin 210-2
```
db.nation.aggregate(EJSON.deserialize(
[
{"$match":{"$and":[{"n_name":{"$in":["CANADA","JAPAN","UNITED KINGDOM"]}},{"$nor":[{"n_name":{"$eq":"ETHIOPIA"}},{"n_regionkey":{"$eq":3}},{"n_regionkey":{"$eq":3}}]}]}},
{"$lookup":{"from":"region","localField":"n_regionkey","foreignField":"r_regionkey","as":"region","pipeline":[
{"$match":{"$or":[{"r_name":{"$eq":"ASIA"}},{"r_regionkey":{"$eq":3}}]}}]}},
{"$unwind":"$region"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$region"]}}},
{"$lookup":{"from":"supplier","localField":"n_nationkey","foreignField":"s_nationkey","as":"supplier","pipeline":[
{"$match":{"$and":[{"s_acctbal":{"$lte":958.07}},{}]}}]}},
{"$unwind":"$supplier"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$supplier"]}}}]
));
```
Subjoin plan:
```
INLJ nation_s.n_nationkey = s_nationkey
  -> [none] HJ n_regionkey = r_regionkey
      -> [nation_s] COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"$and":[{"n_name":{"$in":["CANADA","JAPAN","UNITED KINGDOM"]}},{"$nor":[{"n_name":{"$eq":"ETHIOPIA"}},{"n_regionkey":{"$eq":3}},{"n_regionkey":{"$eq":3}}]}]} 
      -> [region_s] COLLSCAN: plan_stability_subjoin_cardinality_md.region {"$or":[{"r_name":{"$eq":"ASIA"}},{"r_regionkey":{"$eq":3}}]} 
  -> [none] FETCH: plan_stability_subjoin_cardinality_md.supplier {"s_acctbal":{"$lte":958.07}} 
      -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.supplier s_nationkey_1
```
Estimated cardinality: 6  
Actual cardinality: 9  
Orders of magnitude: 0

---
## >>> Command idx 211
```
{"aggregate":"lineitem","pipeline":[
{"$lookup":{"from":"orders","localField":"l_orderkey","foreignField":"o_orderkey","pipeline":[
{"$match":{"$and":[{"o_orderdate":{"$gt":"1992-06-03T00:00:00.000Z"}},{"o_clerk":"Clerk#000000849"}]}},
{"$match":{"$or":[{"o_orderdate":{"$lt":"1993-12-19T00:00:00.000Z"}}]}}],"as":"orders"}},
{"$unwind":"$orders"},
{"$lookup":{"from":"customer","localField":"orders.o_custkey","foreignField":"c_custkey","pipeline":[
{"$match":{"$or":[{"c_acctbal":{"$gt":-624.49}},{"c_name":"Customer#000005090"}]}}],"as":"customer"}},
{"$unwind":"$customer"},
{"$lookup":{"from":"supplier","localField":"customer.c_nationkey","foreignField":"s_nationkey","pipeline":[
{"$match":{"$or":[{"s_acctbal":{"$lt":9537.73}},{"s_acctbal":{"$gte":7448.46}},{"s_name":"Supplier#000000920"}]}},
{"$match":{"$or":[{"s_acctbal":{"$gt":6089.75}},{"s_name":"Supplier#000000836"},{"s_nationkey":23}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$match":{"$nor":[{"l_shipdate":{"$lte":"1994-01-25T00:00:00.000Z"}}]}}],"cursor":{},"idx":211}
```
### >>> Subjoin 211-0
```
db.orders.aggregate(EJSON.deserialize(
[
{"$match":{"$and":[{"o_clerk":{"$eq":"Clerk#000000849"}},{"o_orderdate":{"$gt":"1992-06-03T00:00:00.000Z","$lt":"1993-12-19T00:00:00.000Z"}}]}}]
));
```
Subjoin plan:
```
FETCH: plan_stability_subjoin_cardinality_md.orders {"o_clerk":{"$eq":"Clerk#000000849"}} 
  -> IXSCAN: plan_stability_subjoin_cardinality_md.orders o_orderdate_1 {"o_orderdate":["(new Date(707529600000), new Date(756259200000))"]}
```
Estimated cardinality: 0  
Actual cardinality: 32  
Orders of magnitude: 1

---
### >>> Subjoin 211-1
```
db.orders.aggregate(EJSON.deserialize(
[
{"$match":{"$and":[{"o_clerk":{"$eq":"Clerk#000000849"}},{"o_orderdate":{"$gt":"1992-06-03T00:00:00.000Z","$lt":"1993-12-19T00:00:00.000Z"}}]}},
{"$lookup":{"from":"lineitem","localField":"o_orderkey","foreignField":"l_orderkey","as":"lineitem","pipeline":[
{"$match":{"$and":[{"l_shipdate":{"$not":{"$lte":"1994-01-25T00:00:00.000Z"}}},{}]}}]}},
{"$unwind":"$lineitem"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$lineitem"]}}}]
));
```
Subjoin plan:
```
INLJ o_orderkey = l_orderkey
  -> [orders] FETCH: plan_stability_subjoin_cardinality_md.orders {"o_clerk":{"$eq":"Clerk#000000849"}} 
      -> IXSCAN: plan_stability_subjoin_cardinality_md.orders o_orderdate_1 {"o_orderdate":["(new Date(707529600000), new Date(756259200000))"]}
  -> [none] FETCH: plan_stability_subjoin_cardinality_md.lineitem {"l_shipdate":{"$not":{"$lte":"1994-01-25T00:00:00.000Z"}}} 
      -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.lineitem l_orderkey_1
```
Estimated cardinality: 0  
Actual cardinality: 11  
Orders of magnitude: 1

---
### >>> Subjoin 211-2
```
db.orders.aggregate(EJSON.deserialize(
[
{"$match":{"$and":[{"o_clerk":{"$eq":"Clerk#000000849"}},{"o_orderdate":{"$gt":"1992-06-03T00:00:00.000Z","$lt":"1993-12-19T00:00:00.000Z"}}]}},
{"$lookup":{"from":"lineitem","localField":"o_orderkey","foreignField":"l_orderkey","as":"lineitem","pipeline":[
{"$match":{"$and":[{"l_shipdate":{"$not":{"$lte":"1994-01-25T00:00:00.000Z"}}},{}]}}]}},
{"$unwind":"$lineitem"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$lineitem"]}}},
{"$lookup":{"from":"customer","localField":"o_custkey","foreignField":"c_custkey","as":"customer","pipeline":[
{"$match":{"$and":[{"$or":[{"c_name":{"$eq":"Customer#000005090"}},{"c_acctbal":{"$gt":-624.49}}]},{}]}}]}},
{"$unwind":"$customer"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$customer"]}}}]
));
```
Subjoin plan:
```
INLJ orders.o_custkey = c_custkey
  -> [none] INLJ o_orderkey = l_orderkey
      -> [orders] FETCH: plan_stability_subjoin_cardinality_md.orders {"o_clerk":{"$eq":"Clerk#000000849"}} 
          -> IXSCAN: plan_stability_subjoin_cardinality_md.orders o_orderdate_1 {"o_orderdate":["(new Date(707529600000), new Date(756259200000))"]}
      -> [none] FETCH: plan_stability_subjoin_cardinality_md.lineitem {"l_shipdate":{"$not":{"$lte":"1994-01-25T00:00:00.000Z"}}} 
          -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.lineitem l_orderkey_1
  -> [customer] FETCH: plan_stability_subjoin_cardinality_md.customer {"$or":[{"c_name":{"$eq":"Customer#000005090"}},{"c_acctbal":{"$gt":-624.49}}]} 
      -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.customer c_custkey_1
```
Estimated cardinality: 0  
Actual cardinality: 11  
Orders of magnitude: 1

---
### >>> Subjoin 211-3
```
db.orders.aggregate(EJSON.deserialize(
[
{"$match":{"$and":[{"o_clerk":{"$eq":"Clerk#000000849"}},{"o_orderdate":{"$gt":"1992-06-03T00:00:00.000Z","$lt":"1993-12-19T00:00:00.000Z"}}]}},
{"$lookup":{"from":"lineitem","localField":"o_orderkey","foreignField":"l_orderkey","as":"lineitem","pipeline":[
{"$match":{"$and":[{"l_shipdate":{"$not":{"$lte":"1994-01-25T00:00:00.000Z"}}},{}]}}]}},
{"$unwind":"$lineitem"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$lineitem"]}}},
{"$lookup":{"from":"customer","localField":"o_custkey","foreignField":"c_custkey","as":"customer","pipeline":[
{"$match":{"$and":[{"$or":[{"c_name":{"$eq":"Customer#000005090"}},{"c_acctbal":{"$gt":-624.49}}]},{}]}}]}},
{"$unwind":"$customer"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$customer"]}}},
{"$lookup":{"from":"supplier","localField":"c_nationkey","foreignField":"s_nationkey","as":"supplier","pipeline":[
{"$match":{"$and":[{"$and":[{"$or":[{"s_name":{"$eq":"Supplier#000000836"}},{"s_nationkey":{"$eq":23}},{"s_acctbal":{"$gt":6089.75}}]},{"$or":[{"s_name":{"$eq":"Supplier#000000920"}},{"s_acctbal":{"$lt":9537.73}},{"s_acctbal":{"$gte":7448.46}}]}]},{}]}}]}},
{"$unwind":"$supplier"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$supplier"]}}}]
));
```
Subjoin plan:
```
INLJ customer.c_nationkey = s_nationkey
  -> [none] INLJ orders.o_custkey = c_custkey
      -> [none] INLJ o_orderkey = l_orderkey
          -> [orders] FETCH: plan_stability_subjoin_cardinality_md.orders {"o_clerk":{"$eq":"Clerk#000000849"}} 
              -> IXSCAN: plan_stability_subjoin_cardinality_md.orders o_orderdate_1 {"o_orderdate":["(new Date(707529600000), new Date(756259200000))"]}
          -> [none] FETCH: plan_stability_subjoin_cardinality_md.lineitem {"l_shipdate":{"$not":{"$lte":"1994-01-25T00:00:00.000Z"}}} 
              -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.lineitem l_orderkey_1
      -> [customer] FETCH: plan_stability_subjoin_cardinality_md.customer {"$or":[{"c_name":{"$eq":"Customer#000005090"}},{"c_acctbal":{"$gt":-624.49}}]} 
          -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.customer c_custkey_1
  -> [supplier] FETCH: plan_stability_subjoin_cardinality_md.supplier {"$and":[{"$or":[{"s_name":{"$eq":"Supplier#000000836"}},{"s_nationkey":{"$eq":23}},{"s_acctbal":{"$gt":6089.75}}]},{"$or":[{"s_name":{"$eq":"Supplier#000000920"}},{"s_acctbal":{"$lt":9537.73}},{"s_acctbal":{"$gte":7448.46}}]}]} 
      -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.supplier s_nationkey_1
```
Estimated cardinality: 0  
Actual cardinality: 159  
Orders of magnitude: 2
> [!WARNING]
> Estimate discrepancy is more than 2 orders of magnitude.

---
## >>> Command idx 212
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$or":[{"ps_comment":{"$regex":{"$regex":"^gul","$options":""}}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"supplier","localField":"partsupp.ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$or":[{"s_acctbal":{"$gt":-942.73}},{"s_acctbal":{"$gt":5364.99}}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$nor":[{"n_regionkey":2}]}},
{"$match":{"$nor":[{"n_name":"UNITED KINGDOM"}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$lookup":{"from":"region","localField":"nation_s.n_regionkey","foreignField":"r_regionkey","pipeline":[
{"$match":{"$and":[{"r_name":"MIDDLE EAST"}]}},
{"$match":{"$and":[{"r_name":"MIDDLE EAST"}]}},
{"$match":{"$nor":[{"r_regionkey":0},{"r_name":"ASIA"}]}}],"as":"region_s"}},
{"$unwind":"$region_s"},
{"$match":{"$and":[{"p_type":"LARGE PLATED NICKEL"}]}}],"cursor":{},"idx":212}
```
### >>> Subjoin 212-0
```
db.part.aggregate(EJSON.deserialize(
[
{"$match":{"p_type":{"$eq":"LARGE PLATED NICKEL"}}}]
));
```
Subjoin plan:
```
COLLSCAN: plan_stability_subjoin_cardinality_md.part {"p_type":{"$eq":"LARGE PLATED NICKEL"}}
```
Estimated cardinality: 80  
Actual cardinality: 115  
Orders of magnitude: 1

---
### >>> Subjoin 212-1
```
db.part.aggregate(EJSON.deserialize(
[
{"$match":{"p_type":{"$eq":"LARGE PLATED NICKEL"}}},
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","as":"partsupp","pipeline":[
{"$match":{"$and":[{"ps_comment":{"$regex":"^gul"}},{}]}}]}},
{"$unwind":"$partsupp"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$partsupp"]}}}]
));
```
Subjoin plan:
```
INLJ p_partkey = ps_partkey
  -> [none] COLLSCAN: plan_stability_subjoin_cardinality_md.part {"p_type":{"$eq":"LARGE PLATED NICKEL"}} 
  -> [partsupp] FETCH: plan_stability_subjoin_cardinality_md.partsupp {"ps_comment":{"$regex":"^gul"}} 
      -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.partsupp ps_partkey_1
```
Estimated cardinality: 2  
Actual cardinality: 2  
Orders of magnitude: 0

---
### >>> Subjoin 212-2
```
db.part.aggregate(EJSON.deserialize(
[
{"$match":{"p_type":{"$eq":"LARGE PLATED NICKEL"}}},
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","as":"partsupp","pipeline":[
{"$match":{"$and":[{"ps_comment":{"$regex":"^gul"}},{}]}}]}},
{"$unwind":"$partsupp"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$partsupp"]}}},
{"$lookup":{"from":"supplier","localField":"ps_suppkey","foreignField":"s_suppkey","as":"supplier","pipeline":[
{"$match":{"$and":[{"$or":[{"s_acctbal":{"$gt":-942.73}},{"s_acctbal":{"$gt":5364.99}}]},{}]}}]}},
{"$unwind":"$supplier"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$supplier"]}}}]
));
```
Subjoin plan:
```
INLJ partsupp.ps_suppkey = s_suppkey
  -> [none] INLJ p_partkey = ps_partkey
      -> [none] COLLSCAN: plan_stability_subjoin_cardinality_md.part {"p_type":{"$eq":"LARGE PLATED NICKEL"}} 
      -> [partsupp] FETCH: plan_stability_subjoin_cardinality_md.partsupp {"ps_comment":{"$regex":"^gul"}} 
          -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.partsupp ps_partkey_1
  -> [supplier] FETCH: plan_stability_subjoin_cardinality_md.supplier {"$or":[{"s_acctbal":{"$gt":-942.73}},{"s_acctbal":{"$gt":5364.99}}]} 
      -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.supplier s_suppkey_1
```
Estimated cardinality: 2  
Actual cardinality: 2  
Orders of magnitude: 0

---
### >>> Subjoin 212-3
```
db.part.aggregate(EJSON.deserialize(
[
{"$match":{"p_type":{"$eq":"LARGE PLATED NICKEL"}}},
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","as":"partsupp","pipeline":[
{"$match":{"$and":[{"ps_comment":{"$regex":"^gul"}},{}]}}]}},
{"$unwind":"$partsupp"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$partsupp"]}}},
{"$lookup":{"from":"supplier","localField":"ps_suppkey","foreignField":"s_suppkey","as":"supplier","pipeline":[
{"$match":{"$and":[{"$or":[{"s_acctbal":{"$gt":-942.73}},{"s_acctbal":{"$gt":5364.99}}]},{}]}}]}},
{"$unwind":"$supplier"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$supplier"]}}},
{"$lookup":{"from":"nation","localField":"s_nationkey","foreignField":"n_nationkey","as":"nation","pipeline":[
{"$match":{"$and":[{"n_name":{"$not":{"$eq":"UNITED KINGDOM"}}},{"n_regionkey":{"$not":{"$eq":2}}}]}}]}},
{"$unwind":"$nation"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$nation"]}}}]
));
```
Subjoin plan:
```
HJ supplier.s_nationkey = n_nationkey
  -> [none] INLJ partsupp.ps_suppkey = s_suppkey
      -> [none] INLJ p_partkey = ps_partkey
          -> [none] COLLSCAN: plan_stability_subjoin_cardinality_md.part {"p_type":{"$eq":"LARGE PLATED NICKEL"}} 
          -> [partsupp] FETCH: plan_stability_subjoin_cardinality_md.partsupp {"ps_comment":{"$regex":"^gul"}} 
              -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.partsupp ps_partkey_1
      -> [supplier] FETCH: plan_stability_subjoin_cardinality_md.supplier {"$or":[{"s_acctbal":{"$gt":-942.73}},{"s_acctbal":{"$gt":5364.99}}]} 
          -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.supplier s_suppkey_1
  -> [nation_s] COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"$and":[{"n_name":{"$not":{"$eq":"UNITED KINGDOM"}}},{"n_regionkey":{"$not":{"$eq":2}}}]}
```
Estimated cardinality: 1  
Actual cardinality: 2  
Orders of magnitude: 0

---
### >>> Subjoin 212-4
```
db.part.aggregate(EJSON.deserialize(
[
{"$match":{"p_type":{"$eq":"LARGE PLATED NICKEL"}}},
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","as":"partsupp","pipeline":[
{"$match":{"$and":[{"ps_comment":{"$regex":"^gul"}},{}]}}]}},
{"$unwind":"$partsupp"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$partsupp"]}}},
{"$lookup":{"from":"supplier","localField":"ps_suppkey","foreignField":"s_suppkey","as":"supplier","pipeline":[
{"$match":{"$and":[{"$or":[{"s_acctbal":{"$gt":-942.73}},{"s_acctbal":{"$gt":5364.99}}]},{}]}}]}},
{"$unwind":"$supplier"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$supplier"]}}},
{"$lookup":{"from":"nation","localField":"s_nationkey","foreignField":"n_nationkey","as":"nation","pipeline":[
{"$match":{"$and":[{"n_name":{"$not":{"$eq":"UNITED KINGDOM"}}},{"n_regionkey":{"$not":{"$eq":2}}}]}}]}},
{"$unwind":"$nation"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$nation"]}}},
{"$lookup":{"from":"region","localField":"n_regionkey","foreignField":"r_regionkey","as":"region","pipeline":[
{"$match":{"$and":[{"r_name":{"$eq":"MIDDLE EAST"}},{"r_name":{"$eq":"MIDDLE EAST"}},{"$nor":[{"r_name":{"$eq":"ASIA"}},{"r_regionkey":{"$eq":0}}]}]}}]}},
{"$unwind":"$region"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$region"]}}}]
));
```
Subjoin plan:
```
HJ r_regionkey = nation_s.n_regionkey
  -> [region_s] COLLSCAN: plan_stability_subjoin_cardinality_md.region {"$and":[{"r_name":{"$eq":"MIDDLE EAST"}},{"r_name":{"$eq":"MIDDLE EAST"}},{"$nor":[{"r_name":{"$eq":"ASIA"}},{"r_regionkey":{"$eq":0}}]}]} 
  -> [none] HJ supplier.s_nationkey = n_nationkey
      -> [none] INLJ partsupp.ps_suppkey = s_suppkey
          -> [none] INLJ p_partkey = ps_partkey
              -> [none] COLLSCAN: plan_stability_subjoin_cardinality_md.part {"p_type":{"$eq":"LARGE PLATED NICKEL"}} 
              -> [partsupp] FETCH: plan_stability_subjoin_cardinality_md.partsupp {"ps_comment":{"$regex":"^gul"}} 
                  -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.partsupp ps_partkey_1
          -> [supplier] FETCH: plan_stability_subjoin_cardinality_md.supplier {"$or":[{"s_acctbal":{"$gt":-942.73}},{"s_acctbal":{"$gt":5364.99}}]} 
              -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.supplier s_suppkey_1
      -> [nation_s] COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"$and":[{"n_name":{"$not":{"$eq":"UNITED KINGDOM"}}},{"n_regionkey":{"$not":{"$eq":2}}}]}
```
Estimated cardinality: 0  
Actual cardinality: 1  
Orders of magnitude: 0

---
## >>> Command idx 213
```
{"aggregate":"supplier","pipeline":[
{"$lookup":{"from":"nation","localField":"s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$or":[{"n_name":"ALGERIA"},{"n_regionkey":1}]}},
{"$match":{"$or":[{"n_name":"PERU"}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$lookup":{"from":"region","localField":"nation_s.n_regionkey","foreignField":"r_regionkey","pipeline":[
{"$match":{"$or":[{"r_regionkey":4},{"r_regionkey":1}]}}],"as":"region_s"}},
{"$unwind":"$region_s"},
{"$match":{"$and":[{"s_acctbal":{"$eq":7307.62}}]}}],"cursor":{},"idx":213}
```
### >>> Subjoin 213-0
```
db.nation.aggregate(EJSON.deserialize(
[
{"$match":{"$and":[{"$or":[{"n_name":{"$eq":"ALGERIA"}},{"n_regionkey":{"$eq":1}}]},{"n_name":{"$eq":"PERU"}}]}}]
));
```
Subjoin plan:
```
COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"$and":[{"$or":[{"n_name":{"$eq":"ALGERIA"}},{"n_regionkey":{"$eq":1}}]},{"n_name":{"$eq":"PERU"}}]}
```
Estimated cardinality: 1  
Actual cardinality: 1  
Orders of magnitude: 0

---
### >>> Subjoin 213-1
```
db.nation.aggregate(EJSON.deserialize(
[
{"$match":{"$and":[{"$or":[{"n_name":{"$eq":"ALGERIA"}},{"n_regionkey":{"$eq":1}}]},{"n_name":{"$eq":"PERU"}}]}},
{"$lookup":{"from":"region","localField":"n_regionkey","foreignField":"r_regionkey","as":"region","pipeline":[
{"$match":{"r_regionkey":{"$in":[1,4]}}}]}},
{"$unwind":"$region"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$region"]}}}]
));
```
Subjoin plan:
```
NLJ n_regionkey = r_regionkey
  -> [nation_s] COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"$and":[{"$or":[{"n_name":{"$eq":"ALGERIA"}},{"n_regionkey":{"$eq":1}}]},{"n_name":{"$eq":"PERU"}}]} 
  -> [region_s] COLLSCAN: plan_stability_subjoin_cardinality_md.region {"r_regionkey":{"$in":[1,4]}}
```
Estimated cardinality: 0  
Actual cardinality: 1  
Orders of magnitude: 0

---
### >>> Subjoin 213-2
```
db.nation.aggregate(EJSON.deserialize(
[
{"$match":{"$and":[{"$or":[{"n_name":{"$eq":"ALGERIA"}},{"n_regionkey":{"$eq":1}}]},{"n_name":{"$eq":"PERU"}}]}},
{"$lookup":{"from":"region","localField":"n_regionkey","foreignField":"r_regionkey","as":"region","pipeline":[
{"$match":{"r_regionkey":{"$in":[1,4]}}}]}},
{"$unwind":"$region"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$region"]}}},
{"$lookup":{"from":"supplier","localField":"n_nationkey","foreignField":"s_nationkey","as":"supplier","pipeline":[
{"$match":{"$and":[{"s_acctbal":{"$eq":7307.62}},{}]}}]}},
{"$unwind":"$supplier"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$supplier"]}}}]
));
```
Subjoin plan:
```
INLJ nation_s.n_nationkey = s_nationkey
  -> [none] NLJ n_regionkey = r_regionkey
      -> [nation_s] COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"$and":[{"$or":[{"n_name":{"$eq":"ALGERIA"}},{"n_regionkey":{"$eq":1}}]},{"n_name":{"$eq":"PERU"}}]} 
      -> [region_s] COLLSCAN: plan_stability_subjoin_cardinality_md.region {"r_regionkey":{"$in":[1,4]}} 
  -> [none] FETCH: plan_stability_subjoin_cardinality_md.supplier {"s_acctbal":{"$eq":7307.62}} 
      -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.supplier s_nationkey_1
```
Estimated cardinality: 0  
Actual cardinality: 1  
Orders of magnitude: 0

---
## >>> Command idx 214
```
{"aggregate":"lineitem","pipeline":[
{"$lookup":{"from":"orders","localField":"l_orderkey","foreignField":"o_orderkey","pipeline":[
{"$match":{"$nor":[{"o_clerk":"Clerk#000000013"},{"o_orderdate":{"$eq":"1995-07-14T00:00:00.000Z"}}]}},
{"$match":{"$and":[{"o_orderdate":{"$lt":"1992-01-19T00:00:00.000Z"}}]}}],"as":"orders"}},
{"$unwind":"$orders"},
{"$lookup":{"from":"customer","localField":"orders.o_custkey","foreignField":"c_custkey","pipeline":[
{"$match":{"$and":[{"c_acctbal":{"$gte":-588.23}}]}},
{"$match":{"$and":[{"c_acctbal":{"$gte":6089.13}}]}}],"as":"customer"}},
{"$unwind":"$customer"},
{"$lookup":{"from":"supplier","localField":"customer.c_nationkey","foreignField":"s_nationkey","pipeline":[
{"$match":{"$nor":[{"s_name":"Supplier#000000469"},{"s_acctbal":{"$gte":10.33}}]}},
{"$match":{"$or":[{"s_name":"Supplier#000000766"}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$match":{"$nor":[{"supplier.o_clerk":"Clerk#000000591"}]}}],"cursor":{},"idx":214}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 215
```
{"aggregate":"lineitem","pipeline":[
{"$lookup":{"from":"orders","localField":"l_orderkey","foreignField":"o_orderkey","pipeline":[
{"$match":{"$nor":[{"o_orderdate":{"$lte":"1998-07-29T00:00:00.000Z"}}]}}],"as":"orders"}},
{"$unwind":"$orders"},
{"$lookup":{"from":"customer","localField":"orders.o_custkey","foreignField":"c_custkey","pipeline":[
{"$match":{"$or":[{"c_mktsegment":"MACHINERY"},{"c_name":"Customer#000013959"},{"c_acctbal":{"$gte":9523.62}}]}}],"as":"customer"}},
{"$unwind":"$customer"},
{"$lookup":{"from":"supplier","localField":"customer.c_nationkey","foreignField":"s_nationkey","pipeline":[
{"$match":{"$nor":[{"s_acctbal":{"$gt":-334.52}},{"s_acctbal":{"$eq":-170.22}},{"s_nationkey":24}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$match":{"$nor":[{"l_shipinstruct":{"$in":["TAKE BACK RETURN","TAKE BACK RETURN"]}},{"l_suppkey":771}]}}],"cursor":{},"idx":215}
```
### >>> Subjoin 215-0
```
db.orders.aggregate(EJSON.deserialize(
[
{"$match":{"$or":[{"o_orderdate":{"$lt":null}},{"o_orderdate":{"$gt":"1998-07-29T00:00:00.000Z"}}]}}]
));
```
Subjoin plan:
```
FETCH: plan_stability_subjoin_cardinality_md.orders 
  -> IXSCAN: plan_stability_subjoin_cardinality_md.orders o_orderdate_1 {"o_orderdate":["[MinKey, new Date(-9223372036854775808))","(new Date(901670400000), MaxKey]"]}
```
Estimated cardinality: 150  
Actual cardinality: 242  
Orders of magnitude: 0

---
### >>> Subjoin 215-1
```
db.orders.aggregate(EJSON.deserialize(
[
{"$match":{"$or":[{"o_orderdate":{"$lt":null}},{"o_orderdate":{"$gt":"1998-07-29T00:00:00.000Z"}}]}},
{"$lookup":{"from":"customer","localField":"o_custkey","foreignField":"c_custkey","as":"customer","pipeline":[
{"$match":{"$or":[{"c_mktsegment":{"$eq":"MACHINERY"}},{"c_name":{"$eq":"Customer#000013959"}},{"c_acctbal":{"$gte":9523.62}}]}}]}},
{"$unwind":"$customer"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$customer"]}}}]
));
```
Subjoin plan:
```
HJ o_custkey = c_custkey
  -> [orders] FETCH: plan_stability_subjoin_cardinality_md.orders 
      -> IXSCAN: plan_stability_subjoin_cardinality_md.orders o_orderdate_1 {"o_orderdate":["[MinKey, new Date(-9223372036854775808))","(new Date(901670400000), MaxKey]"]}
  -> [customer] COLLSCAN: plan_stability_subjoin_cardinality_md.customer {"$or":[{"c_mktsegment":{"$eq":"MACHINERY"}},{"c_name":{"$eq":"Customer#000013959"}},{"c_acctbal":{"$gte":9523.62}}]}
```
Estimated cardinality: 31  
Actual cardinality: 67  
Orders of magnitude: 0

---
### >>> Subjoin 215-2
```
db.orders.aggregate(EJSON.deserialize(
[
{"$match":{"$or":[{"o_orderdate":{"$lt":null}},{"o_orderdate":{"$gt":"1998-07-29T00:00:00.000Z"}}]}},
{"$lookup":{"from":"customer","localField":"o_custkey","foreignField":"c_custkey","as":"customer","pipeline":[
{"$match":{"$or":[{"c_mktsegment":{"$eq":"MACHINERY"}},{"c_name":{"$eq":"Customer#000013959"}},{"c_acctbal":{"$gte":9523.62}}]}}]}},
{"$unwind":"$customer"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$customer"]}}},
{"$lookup":{"from":"lineitem","localField":"o_orderkey","foreignField":"l_orderkey","as":"lineitem","pipeline":[
{"$match":{"$and":[{"$nor":[{"l_shipinstruct":{"$eq":"TAKE BACK RETURN"}},{"l_suppkey":{"$eq":771}}]},{}]}}]}},
{"$unwind":"$lineitem"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$lineitem"]}}}]
));
```
Subjoin plan:
```
INLJ orders.o_orderkey = l_orderkey
  -> [none] HJ o_custkey = c_custkey
      -> [orders] FETCH: plan_stability_subjoin_cardinality_md.orders 
          -> IXSCAN: plan_stability_subjoin_cardinality_md.orders o_orderdate_1 {"o_orderdate":["[MinKey, new Date(-9223372036854775808))","(new Date(901670400000), MaxKey]"]}
      -> [customer] COLLSCAN: plan_stability_subjoin_cardinality_md.customer {"$or":[{"c_mktsegment":{"$eq":"MACHINERY"}},{"c_name":{"$eq":"Customer#000013959"}},{"c_acctbal":{"$gte":9523.62}}]} 
  -> [none] FETCH: plan_stability_subjoin_cardinality_md.lineitem {"$nor":[{"l_shipinstruct":{"$eq":"TAKE BACK RETURN"}},{"l_suppkey":{"$eq":771}}]} 
      -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.lineitem l_orderkey_1
```
Estimated cardinality: 97  
Actual cardinality: 214  
Orders of magnitude: 1

---
### >>> Subjoin 215-3
```
db.orders.aggregate(EJSON.deserialize(
[
{"$match":{"$or":[{"o_orderdate":{"$lt":null}},{"o_orderdate":{"$gt":"1998-07-29T00:00:00.000Z"}}]}},
{"$lookup":{"from":"customer","localField":"o_custkey","foreignField":"c_custkey","as":"customer","pipeline":[
{"$match":{"$or":[{"c_mktsegment":{"$eq":"MACHINERY"}},{"c_name":{"$eq":"Customer#000013959"}},{"c_acctbal":{"$gte":9523.62}}]}}]}},
{"$unwind":"$customer"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$customer"]}}},
{"$lookup":{"from":"lineitem","localField":"o_orderkey","foreignField":"l_orderkey","as":"lineitem","pipeline":[
{"$match":{"$and":[{"$nor":[{"l_shipinstruct":{"$eq":"TAKE BACK RETURN"}},{"l_suppkey":{"$eq":771}}]},{}]}}]}},
{"$unwind":"$lineitem"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$lineitem"]}}},
{"$lookup":{"from":"supplier","localField":"c_nationkey","foreignField":"s_nationkey","as":"supplier","pipeline":[
{"$match":{"$nor":[{"s_acctbal":{"$eq":-170.22}},{"s_nationkey":{"$eq":24}},{"s_acctbal":{"$gt":-334.52}}]}}]}},
{"$unwind":"$supplier"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$supplier"]}}}]
));
```
Subjoin plan:
```
HJ s_nationkey = customer.c_nationkey
  -> [supplier] COLLSCAN: plan_stability_subjoin_cardinality_md.supplier {"$nor":[{"s_acctbal":{"$eq":-170.22}},{"s_nationkey":{"$eq":24}},{"s_acctbal":{"$gt":-334.52}}]} 
  -> [none] INLJ orders.o_orderkey = l_orderkey
      -> [none] HJ o_custkey = c_custkey
          -> [orders] FETCH: plan_stability_subjoin_cardinality_md.orders 
              -> IXSCAN: plan_stability_subjoin_cardinality_md.orders o_orderdate_1 {"o_orderdate":["[MinKey, new Date(-9223372036854775808))","(new Date(901670400000), MaxKey]"]}
          -> [customer] COLLSCAN: plan_stability_subjoin_cardinality_md.customer {"$or":[{"c_mktsegment":{"$eq":"MACHINERY"}},{"c_name":{"$eq":"Customer#000013959"}},{"c_acctbal":{"$gte":9523.62}}]} 
      -> [none] FETCH: plan_stability_subjoin_cardinality_md.lineitem {"$nor":[{"l_shipinstruct":{"$eq":"TAKE BACK RETURN"}},{"l_suppkey":{"$eq":771}}]} 
          -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.lineitem l_orderkey_1
```
Estimated cardinality: 228  
Actual cardinality: 495  
Orders of magnitude: 0

---
## >>> Command idx 216
```
{"aggregate":"partsupp","pipeline":[
{"$lookup":{"from":"supplier","localField":"ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$or":[{"s_acctbal":{"$eq":-334.52}},{"s_acctbal":{"$eq":8724.42}}]}},
{"$match":{"$or":[{"s_nationkey":20},{"s_name":"Supplier#000000690"}]}},
{"$match":{"$or":[{"s_acctbal":{"$lt":7619.85}},{"s_nationkey":7}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$nor":[{"n_name":{"$nin":["PERU","EGYPT","SAUDI ARABIA"]}}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$lookup":{"from":"region","localField":"nation_s.n_regionkey","foreignField":"r_regionkey","pipeline":[
{"$match":{"$nor":[{"r_name":"AFRICA"},{"r_name":"AFRICA"},{"r_regionkey":0},{"r_regionkey":2}]}},
{"$match":{"$or":[{"r_regionkey":4}]}}],"as":"region_s"}},
{"$unwind":"$region_s"},
{"$match":{"$and":[{"ps_supplycost":{"$gte":27.55}}]}}],"cursor":{},"idx":216}
```
### >>> Subjoin 216-0
```
db.region.aggregate(EJSON.deserialize(
[
{"$match":{"$and":[{"r_regionkey":{"$eq":4}},{"$nor":[{"r_name":{"$eq":"AFRICA"}},{"r_name":{"$eq":"AFRICA"}},{"r_regionkey":{"$eq":0}},{"r_regionkey":{"$eq":2}}]}]}}]
));
```
Subjoin plan:
```
COLLSCAN: plan_stability_subjoin_cardinality_md.region {"$and":[{"r_regionkey":{"$eq":4}},{"$nor":[{"r_name":{"$eq":"AFRICA"}},{"r_name":{"$eq":"AFRICA"}},{"r_regionkey":{"$eq":0}},{"r_regionkey":{"$eq":2}}]}]}
```
Estimated cardinality: 1  
Actual cardinality: 1  
Orders of magnitude: 0

---
### >>> Subjoin 216-1
```
db.region.aggregate(EJSON.deserialize(
[
{"$match":{"$and":[{"r_regionkey":{"$eq":4}},{"$nor":[{"r_name":{"$eq":"AFRICA"}},{"r_name":{"$eq":"AFRICA"}},{"r_regionkey":{"$eq":0}},{"r_regionkey":{"$eq":2}}]}]}},
{"$lookup":{"from":"nation","localField":"r_regionkey","foreignField":"n_regionkey","as":"nation","pipeline":[
{"$match":{"$nor":[{"n_name":{"$not":{"$in":["EGYPT","PERU","SAUDI ARABIA"]}}}]}}]}},
{"$unwind":"$nation"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$nation"]}}}]
));
```
Subjoin plan:
```
NLJ r_regionkey = n_regionkey
  -> [region_s] COLLSCAN: plan_stability_subjoin_cardinality_md.region {"$and":[{"r_regionkey":{"$eq":4}},{"$nor":[{"r_name":{"$eq":"AFRICA"}},{"r_name":{"$eq":"AFRICA"}},{"r_regionkey":{"$eq":0}},{"r_regionkey":{"$eq":2}}]}]} 
  -> [nation_s] COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"$nor":[{"n_name":{"$not":{"$in":["EGYPT","PERU","SAUDI ARABIA"]}}}]}
```
Estimated cardinality: 1  
Actual cardinality: 2  
Orders of magnitude: 0

---
### >>> Subjoin 216-2
```
db.region.aggregate(EJSON.deserialize(
[
{"$match":{"$and":[{"r_regionkey":{"$eq":4}},{"$nor":[{"r_name":{"$eq":"AFRICA"}},{"r_name":{"$eq":"AFRICA"}},{"r_regionkey":{"$eq":0}},{"r_regionkey":{"$eq":2}}]}]}},
{"$lookup":{"from":"nation","localField":"r_regionkey","foreignField":"n_regionkey","as":"nation","pipeline":[
{"$match":{"$nor":[{"n_name":{"$not":{"$in":["EGYPT","PERU","SAUDI ARABIA"]}}}]}}]}},
{"$unwind":"$nation"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$nation"]}}},
{"$lookup":{"from":"supplier","localField":"n_nationkey","foreignField":"s_nationkey","as":"supplier","pipeline":[
{"$match":{"$and":[{"$and":[{"$or":[{"s_name":{"$eq":"Supplier#000000690"}},{"s_nationkey":{"$eq":20}}]},{"$or":[{"s_nationkey":{"$eq":7}},{"s_acctbal":{"$lt":7619.85}}]},{"s_acctbal":{"$in":[-334.52,8724.42]}}]},{}]}}]}},
{"$unwind":"$supplier"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$supplier"]}}}]
));
```
Subjoin plan:
```
INLJ nation_s.n_nationkey = s_nationkey
  -> [none] NLJ r_regionkey = n_regionkey
      -> [region_s] COLLSCAN: plan_stability_subjoin_cardinality_md.region {"$and":[{"r_regionkey":{"$eq":4}},{"$nor":[{"r_name":{"$eq":"AFRICA"}},{"r_name":{"$eq":"AFRICA"}},{"r_regionkey":{"$eq":0}},{"r_regionkey":{"$eq":2}}]}]} 
      -> [nation_s] COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"$nor":[{"n_name":{"$not":{"$in":["EGYPT","PERU","SAUDI ARABIA"]}}}]} 
  -> [supplier] FETCH: plan_stability_subjoin_cardinality_md.supplier {"$and":[{"$or":[{"s_name":{"$eq":"Supplier#000000690"}},{"s_nationkey":{"$eq":20}}]},{"$or":[{"s_nationkey":{"$eq":7}},{"s_acctbal":{"$lt":7619.85}}]},{"s_acctbal":{"$in":[-334.52,8724.42]}}]} 
      -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.supplier s_nationkey_1
```
Estimated cardinality: 0  
Actual cardinality: 1  
Orders of magnitude: 0

---
### >>> Subjoin 216-3
```
db.region.aggregate(EJSON.deserialize(
[
{"$match":{"$and":[{"r_regionkey":{"$eq":4}},{"$nor":[{"r_name":{"$eq":"AFRICA"}},{"r_name":{"$eq":"AFRICA"}},{"r_regionkey":{"$eq":0}},{"r_regionkey":{"$eq":2}}]}]}},
{"$lookup":{"from":"nation","localField":"r_regionkey","foreignField":"n_regionkey","as":"nation","pipeline":[
{"$match":{"$nor":[{"n_name":{"$not":{"$in":["EGYPT","PERU","SAUDI ARABIA"]}}}]}}]}},
{"$unwind":"$nation"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$nation"]}}},
{"$lookup":{"from":"supplier","localField":"n_nationkey","foreignField":"s_nationkey","as":"supplier","pipeline":[
{"$match":{"$and":[{"$and":[{"$or":[{"s_name":{"$eq":"Supplier#000000690"}},{"s_nationkey":{"$eq":20}}]},{"$or":[{"s_nationkey":{"$eq":7}},{"s_acctbal":{"$lt":7619.85}}]},{"s_acctbal":{"$in":[-334.52,8724.42]}}]},{}]}}]}},
{"$unwind":"$supplier"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$supplier"]}}},
{"$lookup":{"from":"partsupp","localField":"s_suppkey","foreignField":"ps_suppkey","as":"partsupp","pipeline":[
{"$match":{"$and":[{"ps_supplycost":{"$gte":27.55}},{}]}}]}},
{"$unwind":"$partsupp"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$partsupp"]}}}]
));
```
Subjoin plan:
```
INLJ supplier.s_suppkey = ps_suppkey
  -> [none] INLJ nation_s.n_nationkey = s_nationkey
      -> [none] NLJ r_regionkey = n_regionkey
          -> [region_s] COLLSCAN: plan_stability_subjoin_cardinality_md.region {"$and":[{"r_regionkey":{"$eq":4}},{"$nor":[{"r_name":{"$eq":"AFRICA"}},{"r_name":{"$eq":"AFRICA"}},{"r_regionkey":{"$eq":0}},{"r_regionkey":{"$eq":2}}]}]} 
          -> [nation_s] COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"$nor":[{"n_name":{"$not":{"$in":["EGYPT","PERU","SAUDI ARABIA"]}}}]} 
      -> [supplier] FETCH: plan_stability_subjoin_cardinality_md.supplier {"$and":[{"$or":[{"s_name":{"$eq":"Supplier#000000690"}},{"s_nationkey":{"$eq":20}}]},{"$or":[{"s_nationkey":{"$eq":7}},{"s_acctbal":{"$lt":7619.85}}]},{"s_acctbal":{"$in":[-334.52,8724.42]}}]} 
          -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.supplier s_nationkey_1
  -> [none] FETCH: plan_stability_subjoin_cardinality_md.partsupp {"ps_supplycost":{"$gte":27.55}} 
      -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.partsupp ps_suppkey_1
```
Estimated cardinality: 2  
Actual cardinality: 80  
Orders of magnitude: 1

---
## >>> Command idx 217
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$or":[{"ps_availqty":{"$eq":7813}},{"ps_availqty":{"$eq":8163}},{"ps_comment":{"$regex":{"$regex":"^ith","$options":""}}}]}},
{"$match":{"$nor":[{"ps_availqty":{"$eq":2592}},{"ps_availqty":{"$lt":3930}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"lineitem","localField":"partsupp.ps_partkey","foreignField":"l_partkey","pipeline":[
{"$match":{"$or":[{"l_suppkey":94}]}}],"as":"lineitem"}},
{"$unwind":"$lineitem"},
{"$match":{"$or":[{"lineitem.l_shipinstruct":{"$in":["NONE","COLLECT COD","DELIVER IN PERSON"]}},{"partsupp.ps_supplycost":{"$gt":47.26}},{"lineitem.l_shipmode":{"$in":["RAIL","REG AIR","FOB"]}}]}}],"cursor":{},"idx":217}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 218
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$and":[{"ps_comment":{"$regex":{"$regex":"^pe","$options":""}}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"lineitem","localField":"partsupp.ps_partkey","foreignField":"l_partkey","pipeline":[
{"$match":{"$nor":[{"l_suppkey":543},{"l_orderkey":232065},{"l_commitdate":{"$lt":"1992-11-21T00:00:00.000Z"}},{"l_shipinstruct":"TAKE BACK RETURN"},{"l_shipmode":"TRUCK"},{"l_quantity":{"$eq":35}}]}},
{"$match":{"$and":[{"l_extendedprice":{"$gte":75749.12}},{"l_shipmode":{"$in":["RAIL","MAIL","FOB","REG AIR","RAIL"]}},{"l_shipdate":{"$lt":"1993-05-14T00:00:00.000Z"}}]}}],"as":"lineitem"}},
{"$unwind":"$lineitem"},
{"$match":{"$and":[{"p_container":{"$in":["JUMBO PKG","LG JAR","WRAP DRUM"]}}]}}],"cursor":{},"idx":218}
```
### >>> Subjoin 218-0
```
db.partsupp.aggregate(EJSON.deserialize(
[
{"$match":{"ps_comment":{"$regex":"^pe"}}}]
));
```
Subjoin plan:
```
COLLSCAN: plan_stability_subjoin_cardinality_md.partsupp {"ps_comment":{"$regex":"^pe"}}
```
Estimated cardinality: 400  
Actual cardinality: 508  
Orders of magnitude: 0

---
### >>> Subjoin 218-1
```
db.partsupp.aggregate(EJSON.deserialize(
[
{"$match":{"ps_comment":{"$regex":"^pe"}}},
{"$lookup":{"from":"part","localField":"ps_partkey","foreignField":"p_partkey","as":"part","pipeline":[
{"$match":{"p_container":{"$in":["JUMBO PKG","LG JAR","WRAP DRUM"]}}}]}},
{"$unwind":"$part"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$part"]}}}]
));
```
Subjoin plan:
```
HJ ps_partkey = p_partkey
  -> [partsupp] COLLSCAN: plan_stability_subjoin_cardinality_md.partsupp {"ps_comment":{"$regex":"^pe"}} 
  -> [none] COLLSCAN: plan_stability_subjoin_cardinality_md.part {"p_container":{"$in":["JUMBO PKG","LG JAR","WRAP DRUM"]}}
```
Estimated cardinality: 36  
Actual cardinality: 42  
Orders of magnitude: 0

---
### >>> Subjoin 218-2
```
db.partsupp.aggregate(EJSON.deserialize(
[
{"$match":{"ps_comment":{"$regex":"^pe"}}},
{"$lookup":{"from":"part","localField":"ps_partkey","foreignField":"p_partkey","as":"part","pipeline":[
{"$match":{"p_container":{"$in":["JUMBO PKG","LG JAR","WRAP DRUM"]}}}]}},
{"$unwind":"$part"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$part"]}}},
{"$lookup":{"from":"lineitem","localField":"p_partkey","foreignField":"l_partkey","as":"lineitem","pipeline":[
{"$match":{"$and":[{"$and":[{"l_shipdate":{"$lt":"1993-05-14T00:00:00.000Z"}},{"l_extendedprice":{"$gte":75749.12}},{"l_shipmode":{"$in":["FOB","MAIL","RAIL","REG AIR"]}},{"$nor":[{"l_orderkey":{"$eq":232065}},{"l_quantity":{"$eq":35}},{"l_shipinstruct":{"$eq":"TAKE BACK RETURN"}},{"l_shipmode":{"$eq":"TRUCK"}},{"l_suppkey":{"$eq":543}},{"l_commitdate":{"$lt":"1992-11-21T00:00:00.000Z"}}]}]},{}]}}]}},
{"$unwind":"$lineitem"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$lineitem"]}}}]
));
```
Subjoin plan:
```
INLJ partsupp.ps_partkey = l_partkey, p_partkey = l_partkey
  -> [none] HJ ps_partkey = p_partkey
      -> [partsupp] COLLSCAN: plan_stability_subjoin_cardinality_md.partsupp {"ps_comment":{"$regex":"^pe"}} 
      -> [none] COLLSCAN: plan_stability_subjoin_cardinality_md.part {"p_container":{"$in":["JUMBO PKG","LG JAR","WRAP DRUM"]}} 
  -> [lineitem] FETCH: plan_stability_subjoin_cardinality_md.lineitem {"$and":[{"l_shipdate":{"$lt":"1993-05-14T00:00:00.000Z"}},{"l_extendedprice":{"$gte":75749.12}},{"l_shipmode":{"$in":["FOB","MAIL","RAIL","REG AIR"]}},{"$nor":[{"l_orderkey":{"$eq":232065}},{"l_quantity":{"$eq":35}},{"l_shipinstruct":{"$eq":"TAKE BACK RETURN"}},{"l_shipmode":{"$eq":"TRUCK"}},{"l_suppkey":{"$eq":543}},{"l_commitdate":{"$lt":"1992-11-21T00:00:00.000Z"}}]}]} 
      -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.lineitem l_partkey_1
```
Estimated cardinality: 419  
Actual cardinality: 1  
Orders of magnitude: 2
> [!WARNING]
> Estimate discrepancy is more than 2 orders of magnitude.

---
## >>> Command idx 219
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$nor":[{"ps_comment":{"$regex":{"$regex":"^ am","$options":""}}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"lineitem","localField":"partsupp.ps_partkey","foreignField":"l_partkey","pipeline":[
{"$match":{"$or":[{"l_shipdate":{"$eq":"1998-03-11T00:00:00.000Z"}}]}}],"as":"lineitem"}},
{"$unwind":"$lineitem"},
{"$match":{"$or":[{"p_container":"JUMBO CASE"}]}}],"cursor":{},"idx":219}
```
### >>> Subjoin 219-0
```
db.part.aggregate(EJSON.deserialize(
[
{"$match":{"p_container":{"$eq":"JUMBO CASE"}}}]
));
```
Subjoin plan:
```
COLLSCAN: plan_stability_subjoin_cardinality_md.part {"p_container":{"$eq":"JUMBO CASE"}}
```
Estimated cardinality: 500  
Actual cardinality: 501  
Orders of magnitude: 0

---
### >>> Subjoin 219-1
```
db.part.aggregate(EJSON.deserialize(
[
{"$match":{"p_container":{"$eq":"JUMBO CASE"}}},
{"$lookup":{"from":"lineitem","localField":"p_partkey","foreignField":"l_partkey","as":"lineitem","pipeline":[
{"$match":{"l_shipdate":"1998-03-11T00:00:00.000Z"}}]}},
{"$unwind":"$lineitem"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$lineitem"]}}}]
));
```
Subjoin plan:
```
HJ p_partkey = l_partkey
  -> [none] COLLSCAN: plan_stability_subjoin_cardinality_md.part {"p_container":{"$eq":"JUMBO CASE"}} 
  -> [lineitem] FETCH: plan_stability_subjoin_cardinality_md.lineitem 
      -> IXSCAN: plan_stability_subjoin_cardinality_md.lineitem l_shipdate_1 {"l_shipdate":["[new Date(889574400000), new Date(889574400000)]"]}
```
Estimated cardinality: 15  
Actual cardinality: 6  
Orders of magnitude: 1

---
### >>> Subjoin 219-2
```
db.part.aggregate(EJSON.deserialize(
[
{"$match":{"p_container":{"$eq":"JUMBO CASE"}}},
{"$lookup":{"from":"lineitem","localField":"p_partkey","foreignField":"l_partkey","as":"lineitem","pipeline":[
{"$match":{"l_shipdate":"1998-03-11T00:00:00.000Z"}}]}},
{"$unwind":"$lineitem"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$lineitem"]}}},
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","as":"partsupp","pipeline":[
{"$match":{"$and":[{"ps_comment":{"$not":{"$regex":"^ am"}}},{}]}}]}},
{"$unwind":"$partsupp"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$partsupp"]}}}]
));
```
Subjoin plan:
```
INLJ p_partkey = ps_partkey, lineitem.l_partkey = ps_partkey
  -> [none] HJ p_partkey = l_partkey
      -> [none] COLLSCAN: plan_stability_subjoin_cardinality_md.part {"p_container":{"$eq":"JUMBO CASE"}} 
      -> [lineitem] FETCH: plan_stability_subjoin_cardinality_md.lineitem 
          -> IXSCAN: plan_stability_subjoin_cardinality_md.lineitem l_shipdate_1 {"l_shipdate":["[new Date(889574400000), new Date(889574400000)]"]}
  -> [partsupp] FETCH: plan_stability_subjoin_cardinality_md.partsupp {"ps_comment":{"$not":{"$regex":"^ am"}}} 
      -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.partsupp ps_partkey_1
```
Estimated cardinality: 4705  
Actual cardinality: 24  
Orders of magnitude: 2
> [!WARNING]
> Estimate discrepancy is more than 2 orders of magnitude.

---
## >>> Command idx 220
```
{"aggregate":"supplier","pipeline":[
{"$lookup":{"from":"nation","localField":"s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$and":[{"n_name":{"$in":["EGYPT","ROMANIA"]}}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$lookup":{"from":"region","localField":"nation_s.n_regionkey","foreignField":"r_regionkey","pipeline":[
{"$match":{"$or":[{"r_regionkey":3},{"r_regionkey":1}]}}],"as":"region_s"}},
{"$unwind":"$region_s"},
{"$match":{"$nor":[{"s_name":"Supplier#000000924"},{"s_acctbal":{"$eq":9537.73}}]}}],"cursor":{},"idx":220}
```
### >>> Subjoin 220-0
```
db.nation.aggregate(EJSON.deserialize(
[
{"$match":{"n_name":{"$in":["EGYPT","ROMANIA"]}}}]
));
```
Subjoin plan:
```
COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"n_name":{"$in":["EGYPT","ROMANIA"]}}
```
Estimated cardinality: 2  
Actual cardinality: 2  
Orders of magnitude: 0

---
### >>> Subjoin 220-1
```
db.nation.aggregate(EJSON.deserialize(
[
{"$match":{"n_name":{"$in":["EGYPT","ROMANIA"]}}},
{"$lookup":{"from":"region","localField":"n_regionkey","foreignField":"r_regionkey","as":"region","pipeline":[
{"$match":{"r_regionkey":{"$in":[1,3]}}}]}},
{"$unwind":"$region"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$region"]}}}]
));
```
Subjoin plan:
```
HJ n_regionkey = r_regionkey
  -> [nation_s] COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"n_name":{"$in":["EGYPT","ROMANIA"]}} 
  -> [region_s] COLLSCAN: plan_stability_subjoin_cardinality_md.region {"r_regionkey":{"$in":[1,3]}}
```
Estimated cardinality: 1  
Actual cardinality: 1  
Orders of magnitude: 0

---
### >>> Subjoin 220-2
```
db.nation.aggregate(EJSON.deserialize(
[
{"$match":{"n_name":{"$in":["EGYPT","ROMANIA"]}}},
{"$lookup":{"from":"region","localField":"n_regionkey","foreignField":"r_regionkey","as":"region","pipeline":[
{"$match":{"r_regionkey":{"$in":[1,3]}}}]}},
{"$unwind":"$region"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$region"]}}},
{"$lookup":{"from":"supplier","localField":"n_nationkey","foreignField":"s_nationkey","as":"supplier","pipeline":[
{"$match":{"$and":[{"$nor":[{"s_acctbal":{"$eq":9537.73}},{"s_name":{"$eq":"Supplier#000000924"}}]},{}]}}]}},
{"$unwind":"$supplier"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$supplier"]}}}]
));
```
Subjoin plan:
```
INLJ nation_s.n_nationkey = s_nationkey
  -> [none] HJ n_regionkey = r_regionkey
      -> [nation_s] COLLSCAN: plan_stability_subjoin_cardinality_md.nation {"n_name":{"$in":["EGYPT","ROMANIA"]}} 
      -> [region_s] COLLSCAN: plan_stability_subjoin_cardinality_md.region {"r_regionkey":{"$in":[1,3]}} 
  -> [none] FETCH: plan_stability_subjoin_cardinality_md.supplier {"$nor":[{"s_acctbal":{"$eq":9537.73}},{"s_name":{"$eq":"Supplier#000000924"}}]} 
      -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.supplier s_nationkey_1
```
Estimated cardinality: 32  
Actual cardinality: 33  
Orders of magnitude: 0

---
## >>> Command idx 221
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$nor":[{"ps_supplycost":{"$gte":804.73}},{"ps_comment":{"$regex":{"$regex":"^s","$options":""}}},{"ps_supplycost":{"$lte":780.26}},{"ps_comment":{"$regex":{"$regex":"^ ","$options":""}}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"lineitem","localField":"partsupp.ps_partkey","foreignField":"l_partkey","pipeline":[
{"$match":{"$or":[{"l_suppkey":199}]}}],"as":"lineitem"}},
{"$unwind":"$lineitem"},
{"$match":{"$or":[{"lineitem.l_quantity":{"$lt":23}},{"partsupp.ps_comment":{"$regex":{"$regex":"^y ","$options":""}}}]}}],"cursor":{},"idx":221}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 222
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$and":[{"ps_comment":{"$regex":{"$regex":"^s. ","$options":""}}}]}},
{"$match":{"$or":[{"ps_supplycost":{"$gte":498.13}},{"ps_comment":{"$regex":{"$regex":"^t","$options":""}}}]}},
{"$match":{"$and":[{"ps_availqty":{"$lte":5420}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"lineitem","localField":"partsupp.ps_partkey","foreignField":"l_partkey","pipeline":[
{"$match":{"$or":[{"l_shipmode":"MAIL"}]}}],"as":"lineitem"}},
{"$unwind":"$lineitem"},
{"$match":{"$and":[{"p_mfgr":{"$in":["Manufacturer#1","Manufacturer#1","Manufacturer#4"]}},{"p_type":"MEDIUM BRUSHED STEEL"}]}}],"cursor":{},"idx":222}
```
### >>> Subjoin 222-0
```
db.part.aggregate(EJSON.deserialize(
[
{"$match":{"$and":[{"p_type":{"$eq":"MEDIUM BRUSHED STEEL"}},{"p_mfgr":{"$in":["Manufacturer#1","Manufacturer#4"]}}]}}]
));
```
Subjoin plan:
```
COLLSCAN: plan_stability_subjoin_cardinality_md.part {"$and":[{"p_type":{"$eq":"MEDIUM BRUSHED STEEL"}},{"p_mfgr":{"$in":["Manufacturer#1","Manufacturer#4"]}}]}
```
Estimated cardinality: 80  
Actual cardinality: 45  
Orders of magnitude: 0

---
### >>> Subjoin 222-1
```
db.part.aggregate(EJSON.deserialize(
[
{"$match":{"$and":[{"p_type":{"$eq":"MEDIUM BRUSHED STEEL"}},{"p_mfgr":{"$in":["Manufacturer#1","Manufacturer#4"]}}]}},
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","as":"partsupp","pipeline":[
{"$match":{"$and":[{"$and":[{"$or":[{"ps_supplycost":{"$gte":498.13}},{"ps_comment":{"$regex":"^t"}}]},{"ps_availqty":{"$lte":5420}},{"ps_comment":{"$regex":"^s. "}}]},{}]}}]}},
{"$unwind":"$partsupp"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$partsupp"]}}}]
));
```
Subjoin plan:
```
INLJ p_partkey = ps_partkey
  -> [none] COLLSCAN: plan_stability_subjoin_cardinality_md.part {"$and":[{"p_type":{"$eq":"MEDIUM BRUSHED STEEL"}},{"p_mfgr":{"$in":["Manufacturer#1","Manufacturer#4"]}}]} 
  -> [partsupp] FETCH: plan_stability_subjoin_cardinality_md.partsupp {"$and":[{"$or":[{"ps_supplycost":{"$gte":498.13}},{"ps_comment":{"$regex":"^t"}}]},{"ps_availqty":{"$lte":5420}},{"ps_comment":{"$regex":"^s. "}}]} 
      -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.partsupp ps_partkey_1
```
Estimated cardinality: 1  
Actual cardinality: 2  
Orders of magnitude: 0

---
### >>> Subjoin 222-2
```
db.part.aggregate(EJSON.deserialize(
[
{"$match":{"$and":[{"p_type":{"$eq":"MEDIUM BRUSHED STEEL"}},{"p_mfgr":{"$in":["Manufacturer#1","Manufacturer#4"]}}]}},
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","as":"partsupp","pipeline":[
{"$match":{"$and":[{"$and":[{"$or":[{"ps_supplycost":{"$gte":498.13}},{"ps_comment":{"$regex":"^t"}}]},{"ps_availqty":{"$lte":5420}},{"ps_comment":{"$regex":"^s. "}}]},{}]}}]}},
{"$unwind":"$partsupp"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$partsupp"]}}},
{"$lookup":{"from":"lineitem","localField":"p_partkey","foreignField":"l_partkey","as":"lineitem","pipeline":[
{"$match":{"$and":[{"l_shipmode":{"$eq":"MAIL"}},{}]}}]}},
{"$unwind":"$lineitem"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$lineitem"]}}}]
));
```
Subjoin plan:
```
INLJ partsupp.ps_partkey = l_partkey, p_partkey = l_partkey
  -> [none] INLJ p_partkey = ps_partkey
      -> [none] COLLSCAN: plan_stability_subjoin_cardinality_md.part {"$and":[{"p_type":{"$eq":"MEDIUM BRUSHED STEEL"}},{"p_mfgr":{"$in":["Manufacturer#1","Manufacturer#4"]}}]} 
      -> [partsupp] FETCH: plan_stability_subjoin_cardinality_md.partsupp {"$and":[{"$or":[{"ps_supplycost":{"$gte":498.13}},{"ps_comment":{"$regex":"^t"}}]},{"ps_availqty":{"$lte":5420}},{"ps_comment":{"$regex":"^s. "}}]} 
          -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.partsupp ps_partkey_1
  -> [lineitem] FETCH: plan_stability_subjoin_cardinality_md.lineitem {"l_shipmode":{"$eq":"MAIL"}} 
      -> INDEX_PROBE_NODE: plan_stability_subjoin_cardinality_md.lineitem l_partkey_1
```
Estimated cardinality: 310  
Actual cardinality: 13  
Orders of magnitude: 1

---
## >>> Command idx 223
```
{"aggregate":"part","pipeline":[
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","pipeline":[
{"$match":{"$or":[{"ps_supplycost":{"$gte":587.19}}]}}],"as":"partsupp"}},
{"$unwind":"$partsupp"},
{"$lookup":{"from":"lineitem","localField":"partsupp.ps_partkey","foreignField":"l_partkey","pipeline":[
{"$match":{"$or":[{"l_suppkey":646},{"l_suppkey":943}]}}],"as":"lineitem"}},
{"$unwind":"$lineitem"},
{"$match":{"$or":[{"p_mfgr":{"$nin":["Manufacturer#1","Manufacturer#1","Manufacturer#3","Manufacturer#2"]}}]}}],"cursor":{},"idx":223}
```
### >>> Subjoin 223-0
```
db.lineitem.aggregate(EJSON.deserialize(
[
{"$match":{"l_suppkey":{"$in":[646,943]}}}]
));
```
Subjoin plan:
```
FETCH: plan_stability_subjoin_cardinality_md.lineitem 
  -> IXSCAN: plan_stability_subjoin_cardinality_md.lineitem l_suppkey_1 {"l_suppkey":["[646.0, 646.0]","[943.0, 943.0]"]}
```
Estimated cardinality: 1802  
Actual cardinality: 1283  
Orders of magnitude: 0

---
### >>> Subjoin 223-1
```
db.lineitem.aggregate(EJSON.deserialize(
[
{"$match":{"l_suppkey":{"$in":[646,943]}}},
{"$lookup":{"from":"part","localField":"l_partkey","foreignField":"p_partkey","as":"part","pipeline":[
{"$match":{"p_mfgr":{"$not":{"$in":["Manufacturer#1","Manufacturer#2","Manufacturer#3"]}}}}]}},
{"$unwind":"$part"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$part"]}}}]
));
```
Subjoin plan:
```
HJ l_partkey = p_partkey
  -> [lineitem] FETCH: plan_stability_subjoin_cardinality_md.lineitem 
      -> IXSCAN: plan_stability_subjoin_cardinality_md.lineitem l_suppkey_1 {"l_suppkey":["[646.0, 646.0]","[943.0, 943.0]"]}
  -> [none] COLLSCAN: plan_stability_subjoin_cardinality_md.part {"p_mfgr":{"$not":{"$in":["Manufacturer#1","Manufacturer#2","Manufacturer#3"]}}}
```
Estimated cardinality: 701  
Actual cardinality: 497  
Orders of magnitude: 0

---
### >>> Subjoin 223-2
```
db.lineitem.aggregate(EJSON.deserialize(
[
{"$match":{"l_suppkey":{"$in":[646,943]}}},
{"$lookup":{"from":"part","localField":"l_partkey","foreignField":"p_partkey","as":"part","pipeline":[
{"$match":{"p_mfgr":{"$not":{"$in":["Manufacturer#1","Manufacturer#2","Manufacturer#3"]}}}}]}},
{"$unwind":"$part"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$part"]}}},
{"$lookup":{"from":"partsupp","localField":"p_partkey","foreignField":"ps_partkey","as":"partsupp","pipeline":[
{"$match":{"ps_supplycost":{"$gte":587.19}}}]}},
{"$unwind":"$partsupp"},{"$replaceRoot":{"newRoot":{"$mergeObjects":["$$ROOT","$partsupp"]}}}]
));
```
Subjoin plan:
```
HJ p_partkey = ps_partkey, lineitem.l_partkey = ps_partkey
  -> [none] HJ l_partkey = p_partkey
      -> [lineitem] FETCH: plan_stability_subjoin_cardinality_md.lineitem 
          -> IXSCAN: plan_stability_subjoin_cardinality_md.lineitem l_suppkey_1 {"l_suppkey":["[646.0, 646.0]","[943.0, 943.0]"]}
      -> [none] COLLSCAN: plan_stability_subjoin_cardinality_md.part {"p_mfgr":{"$not":{"$in":["Manufacturer#1","Manufacturer#2","Manufacturer#3"]}}} 
  -> [partsupp] COLLSCAN: plan_stability_subjoin_cardinality_md.partsupp {"ps_supplycost":{"$gte":587.19}}
```
Estimated cardinality: 90350  
Actual cardinality: 708  
Orders of magnitude: 2
> [!WARNING]
> Estimate discrepancy is more than 2 orders of magnitude.

---
## >>> Command idx 224
```
{"aggregate":"partsupp","pipeline":[
{"$lookup":{"from":"supplier","localField":"ps_suppkey","foreignField":"s_suppkey","pipeline":[
{"$match":{"$or":[{"s_nationkey":2},{"s_nationkey":5}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$lookup":{"from":"nation","localField":"supplier.s_nationkey","foreignField":"n_nationkey","pipeline":[
{"$match":{"$nor":[{"n_regionkey":3}]}},
{"$match":{"$and":[{"n_regionkey":1}]}},
{"$match":{"$nor":[{"n_regionkey":2},{"n_regionkey":4}]}}],"as":"nation_s"}},
{"$unwind":"$nation_s"},
{"$match":{"$and":[{"nation_s.n_regionkey":1},{"ps_availqty":{"$lte":2592}}]}}],"cursor":{},"idx":224}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 225
```
{"aggregate":"lineitem","pipeline":[
{"$lookup":{"from":"orders","localField":"l_orderkey","foreignField":"o_orderkey","pipeline":[
{"$match":{"$nor":[{"o_orderstatus":"O"},{"o_shippriority":{"$gt":0}}]}},
{"$match":{"$and":[{"o_orderpriority":"2-HIGH"}]}},
{"$match":{"$or":[{"o_shippriority":{"$gte":0}},{"o_orderpriority":"5-LOW"},{"o_orderdate":{"$gte":"1994-05-04T00:00:00.000Z"}}]}},
{"$match":{"$nor":[{"o_orderdate":{"$gt":"1992-01-19T00:00:00.000Z"}}]}}],"as":"orders"}},
{"$unwind":"$orders"},
{"$lookup":{"from":"customer","localField":"orders.o_custkey","foreignField":"c_custkey","pipeline":[
{"$match":{"$or":[{"c_mktsegment":"FURNITURE"}]}}],"as":"customer"}},
{"$unwind":"$customer"},
{"$lookup":{"from":"supplier","localField":"customer.c_nationkey","foreignField":"s_nationkey","pipeline":[
{"$match":{"$or":[{"s_nationkey":20}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$match":{"$or":[{"l_quantity":{"$lte":27}},{"orders.o_orderstatus":"O"}]}}],"cursor":{},"idx":225}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 226
```
{"aggregate":"lineitem","pipeline":[
{"$lookup":{"from":"orders","localField":"l_orderkey","foreignField":"o_orderkey","pipeline":[
{"$match":{"$and":[{"o_orderdate":{"$lt":"1992-08-16T00:00:00.000Z"}}]}}],"as":"orders"}},
{"$unwind":"$orders"},
{"$lookup":{"from":"customer","localField":"orders.o_custkey","foreignField":"c_custkey","pipeline":[
{"$match":{"$or":[{"c_mktsegment":"MACHINERY"}]}}],"as":"customer"}},
{"$unwind":"$customer"},
{"$lookup":{"from":"supplier","localField":"customer.c_nationkey","foreignField":"s_nationkey","pipeline":[
{"$match":{"$and":[{"s_acctbal":{"$gte":1432.69}}]}},
{"$match":{"$nor":[{"s_nationkey":1},{"s_name":"Supplier#000000719"}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$match":{"$or":[{"customer.c_name":"Customer#000012350"}]}}],"cursor":{},"idx":226}
```
Query is not eligible, as it does not have an SBE-only plan.
## >>> Command idx 227
```
{"aggregate":"lineitem","pipeline":[
{"$lookup":{"from":"orders","localField":"l_orderkey","foreignField":"o_orderkey","pipeline":[
{"$match":{"$nor":[{"o_orderdate":{"$lt":"1997-09-10T00:00:00.000Z"}}]}},
{"$match":{"$or":[{"o_orderstatus":"P"},{"o_totalprice":{"$lt":34828.99}}]}}],"as":"orders"}},
{"$unwind":"$orders"},
{"$lookup":{"from":"customer","localField":"orders.o_custkey","foreignField":"c_custkey","pipeline":[
{"$match":{"$nor":[{"c_name":"Customer#000011646"}]}},
{"$match":{"$or":[{"c_mktsegment":"HOUSEHOLD"},{"c_name":"Customer#000001342"}]}}],"as":"customer"}},
{"$unwind":"$customer"},
{"$lookup":{"from":"supplier","localField":"customer.c_nationkey","foreignField":"s_nationkey","pipeline":[
{"$match":{"$and":[{"s_acctbal":{"$eq":7901.42}}]}}],"as":"supplier"}},
{"$unwind":"$supplier"},
{"$match":{"$and":[{"customer.c_acctbal":{"$lt":8496.37}}]}}],"cursor":{},"idx":227}
```
Query is not eligible, as it does not have an SBE-only plan.
