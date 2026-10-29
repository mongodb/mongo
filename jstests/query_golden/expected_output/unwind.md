## 1. $unwind
### Inserted Docs
```json
{ "_id" : 1, "a" : [ { "b" : { "d" : 3 }, "p" : 4 }, { "b" : { "d" : 5 } }, 6 ], "z" : 7 }
```
```json
{ "_id" : 2, "a" : [ { "b" : { "c" : 3, "d" : 4 }, "p" : 5 }, { "b" : { "c" : { "d" : 6 }, "d" : 7 }, "p" : 8 }, 9 ], "z" : 10 }
```
```json
{ "_id" : 3, "a" : [ { "b" : { "c" : [ 3, { "d" : 4 } ], "d" : 5 }, "p" : 6 }, { "b" : { "c" : [ 7, { "d" : 8 } ], "d" : 9 }, "p" : 10 } ], "z" : 11 }
```
```json
{ "_id" : 4, "a" : [ { "b" : [ { "d" : 3 }, { "d" : 4 }, 5 ], "p" : 6 }, { "b" : [ { "d" : 7 }, { "d" : 8 }, 9 ], "p" : 10 } ], "z" : 11 }
```
```json
{ "_id" : 5, "a" : [ { "b" : [ { "c" : 3, "d" : 4 }, { "c" : 5, "d" : 6 }, 7 ], "p" : 8 }, { "b" : [ { "c" : 9, "d" : 10 }, { "c" : 11, "d" : 12 }, 13 ], "p" : 14 } ], "z" : 15 }
```
```json
{ "_id" : 6, "a" : [ { "b" : [ { "c" : [ 3, { "d" : 4 } ], "d" : 5 }, { "c" : [ 6, { "d" : 7 } ], "d" : 8 }, 9 ], "p" : 10 }, { "b" : [ { "c" : [ 11, { "d" : 12 } ], "d" : 13 }, { "c" : [ 14, { "d" : 15 } ], "d" : 16 }, 17 ], "p" : 18 } ], "z" : 19 }
```
```json
{ "_id" : 7, "a" : [ { "b" : { "d" : 3 } }, { "b" : { "d" : 4 } }, 5 ], "j" : { "p" : 7 }, "z" : 8 }
```
```json
{ "_id" : 8, "a" : [ { "b" : { "d" : 3 } }, { "b" : { "d" : 4 } }, 5 ], "j" : { "k" : 7, "p" : 8 }, "z" : 9 }
```
```json
{ "_id" : 9, "a" : [ { "b" : { "d" : 3 } }, { "b" : { "d" : 4 } }, 5 ], "j" : [ ], "z" : 7 }
```
```json
{ "_id" : 10, "a" : [ { "b" : { "d" : 3 } }, { "b" : { "d" : 4 } }, 5 ], "j" : [ { "k" : 7, "p" : 8 }, { "k" : 9, "p" : 10 }, 11 ], "z" : 7 }
```
```json
{ "_id" : 11, "a" : [ { "b" : { "d" : 3 } }, { "b" : { "d" : 4 } }, 5 ], "j" : { "k" : [ 7, 8 ], "p" : 9 }, "z" : 10 }
```
```json
{ "_id" : 12, "a" : [ { "b" : { "d" : 3 } }, { "b" : { "d" : 4 } }, 5 ], "j" : [ { "k" : [ 7, 8 ], "p" : 9 }, { "k" : [ 10, 11 ], "p" : 12 }, 13 ], "z" : 14 }
```
```json
{ "_id" : 13, "a" : { "b" : [ { "c" : { "e" : 3 }, "p" : 4 }, { "c" : { "e" : 5 } }, 6 ], "y" : 7 }, "z" : 7 }
```
```json
{ "_id" : 14, "a" : { "b" : [ { "c" : { "d" : 3, "e" : 4 }, "p" : 5 }, { "c" : { "d" : { "e" : 6 }, "e" : 7 }, "p" : 8 }, 9 ], "y" : 10 }, "z" : 11 }
```
```json
{ "_id" : 15, "a" : { "b" : [ { "c" : { "d" : [ 3, { "e" : 4 } ], "e" : 5 }, "p" : 6 }, { "c" : { "d" : [ 7, { "e" : 8 } ], "e" : 9 }, "p" : 10 } ], "y" : 11 }, "z" : 12 }
```
```json
{ "_id" : 16, "a" : { "b" : [ { "c" : [ { "e" : 3 }, { "e" : 4 }, 5 ], "p" : 6 }, { "c" : [ { "e" : 7 }, { "e" : 8 }, 9 ], "p" : 10 } ], "y" : 11 }, "z" : 12 }
```
```json
{ "_id" : 17, "a" : { "b" : [ { "c" : [ { "d" : 3, "e" : 4 }, { "d" : 5, "e" : 6 }, 7 ], "p" : 8 }, { "c" : [ { "d" : 9, "e" : 10 }, { "d" : 11, "e" : 12 }, 13 ], "p" : 14 } ], "y" : 15 }, "z" : 16 }
```
```json
{ "_id" : 18, "a" : { "b" : [ { "c" : [ { "d" : [ 3, { "e" : 4 } ], "e" : 5 }, { "d" : [ 6, { "e" : 7 } ], "e" : 8 }, 9 ], "p" : 10 }, { "c" : [ { "d" : [ 11, { "e" : 12 } ], "e" : 13 }, { "d" : [ 14, { "e" : 15 } ], "e" : 16 }, 17 ], "p" : 18 } ], "y" : 19 }, "z" : 20 }
```
```json
{ "_id" : 19, "a" : { "b" : [ { "c" : { "e" : 3 }, "p" : 4 }, { "c" : { "e" : 5 } }, 6 ], "y" : 7 }, "j" : { "p" : 8 }, "z" : 9 }
```
```json
{ "_id" : 20, "a" : { "b" : [ { "c" : { "e" : 3 }, "p" : 4 }, { "c" : { "e" : 5 } }, 6 ], "y" : 7 }, "j" : { "k" : 8, "p" : 9 }, "z" : 10 }
```
```json
{ "_id" : 21, "a" : { "b" : [ { "c" : { "e" : 3 }, "p" : 4 }, { "c" : { "e" : 5 } }, 6 ], "y" : 7 }, "j" : [ ], "z" : 7 }
```
```json
{ "_id" : 22, "a" : { "b" : [ { "c" : { "e" : 3 }, "p" : 4 }, { "c" : { "e" : 5 } }, 6 ], "y" : 7 }, "j" : [ { "k" : 7, "p" : 8 }, { "k" : 9, "p" : 10 }, 11 ], "z" : 7 }
```
```json
{ "_id" : 23, "a" : { "b" : [ { "c" : { "e" : 3 } }, { "c" : { "e" : 4 } }, 5 ], "y" : 6 }, "j" : { "k" : [ 7, 8 ], "p" : 9 }, "z" : 10 }
```
```json
{ "_id" : 24, "a" : { "b" : [ { "c" : { "e" : 3 } }, { "c" : { "e" : 4 } }, 5 ], "y" : 6 }, "j" : [ { "k" : [ 7, 8 ], "p" : 9 }, { "k" : [ 10, 11 ], "p" : 12 }, 13 ], "z" : 14 }
```
```json
{ "_id" : 25, "a" : { "b" : { "c" : [ { "d" : 1 }, { "d" : 2 }, { "e" : 3 } ] } } }
```
### Query Test-0
```json
[ { "$match" : { "_id" : 1 } }, { "$unwind" : { "path" : "$a" } }, { "$project" : { "x" : 0 } } ]
```
### Results Test-0
```json
{ "_id" : 1, "a" : 6, "z" : 7 }
{ "_id" : 1, "a" : { "b" : { "d" : 3 }, "p" : 4 }, "z" : 7 }
{ "_id" : 1, "a" : { "b" : { "d" : 5 } }, "z" : 7 }
```
### Query Test-1
```json
[ { "$match" : { "_id" : 1 } }, { "$unwind" : { "path" : "$a", "includeArrayIndex" : "j" } }, { "$project" : { "x" : 0 } } ]
```
### Results Test-1
```json
{ "_id" : 1, "a" : 6, "j" : NumberLong(2), "z" : 7 }
{ "_id" : 1, "a" : { "b" : { "d" : 3 }, "p" : 4 }, "j" : NumberLong(0), "z" : 7 }
{ "_id" : 1, "a" : { "b" : { "d" : 5 } }, "j" : NumberLong(1), "z" : 7 }
```
### Query Test-2
```json
[ { "$match" : { "_id" : 7 } }, { "$unwind" : { "path" : "$a", "includeArrayIndex" : "j" } }, { "$project" : { "x" : 0 } } ]
```
### Results Test-2
```json
{ "_id" : 7, "a" : 5, "j" : NumberLong(2), "z" : 8 }
{ "_id" : 7, "a" : { "b" : { "d" : 3 } }, "j" : NumberLong(0), "z" : 8 }
{ "_id" : 7, "a" : { "b" : { "d" : 4 } }, "j" : NumberLong(1), "z" : 8 }
```
### Query Test-3
```json
[ { "$match" : { "_id" : 21 } }, { "$unwind" : { "path" : "$a", "includeArrayIndex" : "j" } }, { "$project" : { "x" : 0 } } ]
```
### Results Test-3
```json
{ "_id" : 21, "a" : { "b" : [ { "c" : { "e" : 3 }, "p" : 4 }, { "c" : { "e" : 5 } }, 6 ], "y" : 7 }, "j" : null, "z" : 7 }
```
### Query Test-4
```json
[ { "$match" : { "_id" : 22 } }, { "$unwind" : { "path" : "$a", "includeArrayIndex" : "j" } }, { "$project" : { "x" : 0 } } ]
```
### Results Test-4
```json
{ "_id" : 22, "a" : { "b" : [ { "c" : { "e" : 3 }, "p" : 4 }, { "c" : { "e" : 5 } }, 6 ], "y" : 7 }, "j" : null, "z" : 7 }
```
### Query Test-5
```json
[ { "$match" : { "_id" : 1 } }, { "$unwind" : { "path" : "$a", "includeArrayIndex" : "j.k" } }, { "$project" : { "x" : 0 } } ]
```
### Results Test-5
```json
{ "_id" : 1, "a" : 6, "j" : { "k" : NumberLong(2) }, "z" : 7 }
{ "_id" : 1, "a" : { "b" : { "d" : 3 }, "p" : 4 }, "j" : { "k" : NumberLong(0) }, "z" : 7 }
{ "_id" : 1, "a" : { "b" : { "d" : 5 } }, "j" : { "k" : NumberLong(1) }, "z" : 7 }
```
### Query Test-6
```json
[ { "$match" : { "_id" : 7 } }, { "$unwind" : { "path" : "$a", "includeArrayIndex" : "j.k" } }, { "$project" : { "x" : 0 } } ]
```
### Results Test-6
```json
{ "_id" : 7, "a" : 5, "j" : { "k" : NumberLong(2), "p" : 7 }, "z" : 8 }
{ "_id" : 7, "a" : { "b" : { "d" : 3 } }, "j" : { "k" : NumberLong(0), "p" : 7 }, "z" : 8 }
{ "_id" : 7, "a" : { "b" : { "d" : 4 } }, "j" : { "k" : NumberLong(1), "p" : 7 }, "z" : 8 }
```
### Query Test-7
```json
[ { "$match" : { "_id" : 8 } }, { "$unwind" : { "path" : "$a", "includeArrayIndex" : "j.k" } }, { "$project" : { "x" : 0 } } ]
```
### Results Test-7
```json
{ "_id" : 8, "a" : 5, "j" : { "k" : NumberLong(2), "p" : 8 }, "z" : 9 }
{ "_id" : 8, "a" : { "b" : { "d" : 3 } }, "j" : { "k" : NumberLong(0), "p" : 8 }, "z" : 9 }
{ "_id" : 8, "a" : { "b" : { "d" : 4 } }, "j" : { "k" : NumberLong(1), "p" : 8 }, "z" : 9 }
```
### Query Test-8
```json
[ { "$match" : { "_id" : 9 } }, { "$unwind" : { "path" : "$a", "includeArrayIndex" : "j.k" } }, { "$project" : { "x" : 0 } } ]
```
### Results Test-8
```json
{ "_id" : 9, "a" : 5, "j" : { "k" : NumberLong(2) }, "z" : 7 }
{ "_id" : 9, "a" : { "b" : { "d" : 3 } }, "j" : { "k" : NumberLong(0) }, "z" : 7 }
{ "_id" : 9, "a" : { "b" : { "d" : 4 } }, "j" : { "k" : NumberLong(1) }, "z" : 7 }
```
### Query Test-9
```json
[ { "$match" : { "_id" : 10 } }, { "$unwind" : { "path" : "$a", "includeArrayIndex" : "j.k" } }, { "$project" : { "x" : 0 } } ]
```
### Results Test-9
```json
{ "_id" : 10, "a" : 5, "j" : { "k" : NumberLong(2) }, "z" : 7 }
{ "_id" : 10, "a" : { "b" : { "d" : 3 } }, "j" : { "k" : NumberLong(0) }, "z" : 7 }
{ "_id" : 10, "a" : { "b" : { "d" : 4 } }, "j" : { "k" : NumberLong(1) }, "z" : 7 }
```
### Query Test-10
```json
[ { "$match" : { "_id" : 11 } }, { "$unwind" : { "path" : "$a", "includeArrayIndex" : "j.k" } }, { "$project" : { "x" : 0 } } ]
```
### Results Test-10
```json
{ "_id" : 11, "a" : 5, "j" : { "k" : NumberLong(2), "p" : 9 }, "z" : 10 }
{ "_id" : 11, "a" : { "b" : { "d" : 3 } }, "j" : { "k" : NumberLong(0), "p" : 9 }, "z" : 10 }
{ "_id" : 11, "a" : { "b" : { "d" : 4 } }, "j" : { "k" : NumberLong(1), "p" : 9 }, "z" : 10 }
```
### Query Test-11
```json
[ { "$match" : { "_id" : 12 } }, { "$unwind" : { "path" : "$a", "includeArrayIndex" : "j.k" } }, { "$project" : { "x" : 0 } } ]
```
### Results Test-11
```json
{ "_id" : 12, "a" : 5, "j" : { "k" : NumberLong(2) }, "z" : 14 }
{ "_id" : 12, "a" : { "b" : { "d" : 3 } }, "j" : { "k" : NumberLong(0) }, "z" : 14 }
{ "_id" : 12, "a" : { "b" : { "d" : 4 } }, "j" : { "k" : NumberLong(1) }, "z" : 14 }
```
### Query Test-12
```json
[ { "$match" : { "_id" : 1 } }, { "$unwind" : { "path" : "$a", "includeArrayIndex" : "a" } }, { "$project" : { "x" : 0 } } ]
```
### Results Test-12
```json
{ "_id" : 1, "a" : NumberLong(0), "z" : 7 }
{ "_id" : 1, "a" : NumberLong(1), "z" : 7 }
{ "_id" : 1, "a" : NumberLong(2), "z" : 7 }
```
### Query Test-13
```json
[ { "$match" : { "_id" : 1 } }, { "$unwind" : { "path" : "$a", "includeArrayIndex" : "a.b" } }, { "$project" : { "x" : 0 } } ]
```
### Results Test-13
```json
{ "_id" : 1, "a" : { "b" : NumberLong(0), "p" : 4 }, "z" : 7 }
{ "_id" : 1, "a" : { "b" : NumberLong(1) }, "z" : 7 }
{ "_id" : 1, "a" : { "b" : NumberLong(2) }, "z" : 7 }
```
### Query Test-14
```json
[ { "$match" : { "_id" : 4 } }, { "$unwind" : { "path" : "$a", "includeArrayIndex" : "a.b" } }, { "$project" : { "x" : 0 } } ]
```
### Results Test-14
```json
{ "_id" : 4, "a" : { "b" : NumberLong(0), "p" : 6 }, "z" : 11 }
{ "_id" : 4, "a" : { "b" : NumberLong(1), "p" : 10 }, "z" : 11 }
```
### Query Test-15
```json
[ { "$match" : { "_id" : 1 } }, { "$unwind" : { "path" : "$a", "includeArrayIndex" : "a.b.c" } }, { "$project" : { "x" : 0 } } ]
```
### Results Test-15
```json
{ "_id" : 1, "a" : { "b" : { "c" : NumberLong(0), "d" : 3 }, "p" : 4 }, "z" : 7 }
{ "_id" : 1, "a" : { "b" : { "c" : NumberLong(1), "d" : 5 } }, "z" : 7 }
{ "_id" : 1, "a" : { "b" : { "c" : NumberLong(2) } }, "z" : 7 }
```
### Query Test-16
```json
[ { "$match" : { "_id" : 2 } }, { "$unwind" : { "path" : "$a", "includeArrayIndex" : "a.b.c" } }, { "$project" : { "x" : 0 } } ]
```
### Results Test-16
```json
{ "_id" : 2, "a" : { "b" : { "c" : NumberLong(0), "d" : 4 }, "p" : 5 }, "z" : 10 }
{ "_id" : 2, "a" : { "b" : { "c" : NumberLong(1), "d" : 7 }, "p" : 8 }, "z" : 10 }
{ "_id" : 2, "a" : { "b" : { "c" : NumberLong(2) } }, "z" : 10 }
```
### Query Test-17
```json
[ { "$match" : { "_id" : 3 } }, { "$unwind" : { "path" : "$a", "includeArrayIndex" : "a.b.c" } }, { "$project" : { "x" : 0 } } ]
```
### Results Test-17
```json
{ "_id" : 3, "a" : { "b" : { "c" : NumberLong(0), "d" : 5 }, "p" : 6 }, "z" : 11 }
{ "_id" : 3, "a" : { "b" : { "c" : NumberLong(1), "d" : 9 }, "p" : 10 }, "z" : 11 }
```
### Query Test-18
```json
[ { "$match" : { "_id" : 4 } }, { "$unwind" : { "path" : "$a", "includeArrayIndex" : "a.b.c" } }, { "$project" : { "x" : 0 } } ]
```
### Results Test-18
```json
{ "_id" : 4, "a" : { "b" : { "c" : NumberLong(0) }, "p" : 6 }, "z" : 11 }
{ "_id" : 4, "a" : { "b" : { "c" : NumberLong(1) }, "p" : 10 }, "z" : 11 }
```
### Query Test-19
```json
[ { "$match" : { "_id" : 5 } }, { "$unwind" : { "path" : "$a", "includeArrayIndex" : "a.b.c" } }, { "$project" : { "x" : 0 } } ]
```
### Results Test-19
```json
{ "_id" : 5, "a" : { "b" : { "c" : NumberLong(0) }, "p" : 8 }, "z" : 15 }
{ "_id" : 5, "a" : { "b" : { "c" : NumberLong(1) }, "p" : 14 }, "z" : 15 }
```
### Query Test-20
```json
[ { "$match" : { "_id" : 6 } }, { "$unwind" : { "path" : "$a", "includeArrayIndex" : "a.b.c" } }, { "$project" : { "x" : 0 } } ]
```
### Results Test-20
```json
{ "_id" : 6, "a" : { "b" : { "c" : NumberLong(0) }, "p" : 10 }, "z" : 19 }
{ "_id" : 6, "a" : { "b" : { "c" : NumberLong(1) }, "p" : 18 }, "z" : 19 }
```
### Query Test-21
```json
[ { "$match" : { "_id" : 13 } }, { "$unwind" : { "path" : "$a.b" } }, { "$project" : { "x" : 0 } } ]
```
### Results Test-21
```json
{ "_id" : 13, "a" : { "b" : 6, "y" : 7 }, "z" : 7 }
{ "_id" : 13, "a" : { "b" : { "c" : { "e" : 3 }, "p" : 4 }, "y" : 7 }, "z" : 7 }
{ "_id" : 13, "a" : { "b" : { "c" : { "e" : 5 } }, "y" : 7 }, "z" : 7 }
```
### Query Test-22
```json
[ { "$match" : { "_id" : 13 } }, { "$unwind" : { "path" : "$a.b", "includeArrayIndex" : "j" } }, { "$project" : { "x" : 0 } } ]
```
### Results Test-22
```json
{ "_id" : 13, "a" : { "b" : 6, "y" : 7 }, "j" : NumberLong(2), "z" : 7 }
{ "_id" : 13, "a" : { "b" : { "c" : { "e" : 3 }, "p" : 4 }, "y" : 7 }, "j" : NumberLong(0), "z" : 7 }
{ "_id" : 13, "a" : { "b" : { "c" : { "e" : 5 } }, "y" : 7 }, "j" : NumberLong(1), "z" : 7 }
```
### Query Test-23
```json
[ { "$match" : { "_id" : 19 } }, { "$unwind" : { "path" : "$a.b", "includeArrayIndex" : "j" } }, { "$project" : { "x" : 0 } } ]
```
### Results Test-23
```json
{ "_id" : 19, "a" : { "b" : 6, "y" : 7 }, "j" : NumberLong(2), "z" : 9 }
{ "_id" : 19, "a" : { "b" : { "c" : { "e" : 3 }, "p" : 4 }, "y" : 7 }, "j" : NumberLong(0), "z" : 9 }
{ "_id" : 19, "a" : { "b" : { "c" : { "e" : 5 } }, "y" : 7 }, "j" : NumberLong(1), "z" : 9 }
```
### Query Test-24
```json
[ { "$match" : { "_id" : 21 } }, { "$unwind" : { "path" : "$a.b", "includeArrayIndex" : "j" } }, { "$project" : { "x" : 0 } } ]
```
### Results Test-24
```json
{ "_id" : 21, "a" : { "b" : 6, "y" : 7 }, "j" : NumberLong(2), "z" : 7 }
{ "_id" : 21, "a" : { "b" : { "c" : { "e" : 3 }, "p" : 4 }, "y" : 7 }, "j" : NumberLong(0), "z" : 7 }
{ "_id" : 21, "a" : { "b" : { "c" : { "e" : 5 } }, "y" : 7 }, "j" : NumberLong(1), "z" : 7 }
```
### Query Test-25
```json
[ { "$match" : { "_id" : 22 } }, { "$unwind" : { "path" : "$a.b", "includeArrayIndex" : "j" } }, { "$project" : { "x" : 0 } } ]
```
### Results Test-25
```json
{ "_id" : 22, "a" : { "b" : 6, "y" : 7 }, "j" : NumberLong(2), "z" : 7 }
{ "_id" : 22, "a" : { "b" : { "c" : { "e" : 3 }, "p" : 4 }, "y" : 7 }, "j" : NumberLong(0), "z" : 7 }
{ "_id" : 22, "a" : { "b" : { "c" : { "e" : 5 } }, "y" : 7 }, "j" : NumberLong(1), "z" : 7 }
```
### Query Test-26
```json
[ { "$match" : { "_id" : 13 } }, { "$unwind" : { "path" : "$a.b", "includeArrayIndex" : "j.k" } }, { "$project" : { "x" : 0 } } ]
```
### Results Test-26
```json
{ "_id" : 13, "a" : { "b" : 6, "y" : 7 }, "j" : { "k" : NumberLong(2) }, "z" : 7 }
{ "_id" : 13, "a" : { "b" : { "c" : { "e" : 3 }, "p" : 4 }, "y" : 7 }, "j" : { "k" : NumberLong(0) }, "z" : 7 }
{ "_id" : 13, "a" : { "b" : { "c" : { "e" : 5 } }, "y" : 7 }, "j" : { "k" : NumberLong(1) }, "z" : 7 }
```
### Query Test-27
```json
[ { "$match" : { "_id" : 19 } }, { "$unwind" : { "path" : "$a.b", "includeArrayIndex" : "j.k" } }, { "$project" : { "x" : 0 } } ]
```
### Results Test-27
```json
{ "_id" : 19, "a" : { "b" : 6, "y" : 7 }, "j" : { "k" : NumberLong(2), "p" : 8 }, "z" : 9 }
{ "_id" : 19, "a" : { "b" : { "c" : { "e" : 3 }, "p" : 4 }, "y" : 7 }, "j" : { "k" : NumberLong(0), "p" : 8 }, "z" : 9 }
{ "_id" : 19, "a" : { "b" : { "c" : { "e" : 5 } }, "y" : 7 }, "j" : { "k" : NumberLong(1), "p" : 8 }, "z" : 9 }
```
### Query Test-28
```json
[ { "$match" : { "_id" : 20 } }, { "$unwind" : { "path" : "$a.b", "includeArrayIndex" : "j.k" } }, { "$project" : { "x" : 0 } } ]
```
### Results Test-28
```json
{ "_id" : 20, "a" : { "b" : 6, "y" : 7 }, "j" : { "k" : NumberLong(2), "p" : 9 }, "z" : 10 }
{ "_id" : 20, "a" : { "b" : { "c" : { "e" : 3 }, "p" : 4 }, "y" : 7 }, "j" : { "k" : NumberLong(0), "p" : 9 }, "z" : 10 }
{ "_id" : 20, "a" : { "b" : { "c" : { "e" : 5 } }, "y" : 7 }, "j" : { "k" : NumberLong(1), "p" : 9 }, "z" : 10 }
```
### Query Test-29
```json
[ { "$match" : { "_id" : 21 } }, { "$unwind" : { "path" : "$a.b", "includeArrayIndex" : "j.k" } }, { "$project" : { "x" : 0 } } ]
```
### Results Test-29
```json
{ "_id" : 21, "a" : { "b" : 6, "y" : 7 }, "j" : { "k" : NumberLong(2) }, "z" : 7 }
{ "_id" : 21, "a" : { "b" : { "c" : { "e" : 3 }, "p" : 4 }, "y" : 7 }, "j" : { "k" : NumberLong(0) }, "z" : 7 }
{ "_id" : 21, "a" : { "b" : { "c" : { "e" : 5 } }, "y" : 7 }, "j" : { "k" : NumberLong(1) }, "z" : 7 }
```
### Query Test-30
```json
[ { "$match" : { "_id" : 22 } }, { "$unwind" : { "path" : "$a.b", "includeArrayIndex" : "j.k" } }, { "$project" : { "x" : 0 } } ]
```
### Results Test-30
```json
{ "_id" : 22, "a" : { "b" : 6, "y" : 7 }, "j" : { "k" : NumberLong(2) }, "z" : 7 }
{ "_id" : 22, "a" : { "b" : { "c" : { "e" : 3 }, "p" : 4 }, "y" : 7 }, "j" : { "k" : NumberLong(0) }, "z" : 7 }
{ "_id" : 22, "a" : { "b" : { "c" : { "e" : 5 } }, "y" : 7 }, "j" : { "k" : NumberLong(1) }, "z" : 7 }
```
### Query Test-31
```json
[ { "$match" : { "_id" : 23 } }, { "$unwind" : { "path" : "$a.b", "includeArrayIndex" : "j.k" } }, { "$project" : { "x" : 0 } } ]
```
### Results Test-31
```json
{ "_id" : 23, "a" : { "b" : 5, "y" : 6 }, "j" : { "k" : NumberLong(2), "p" : 9 }, "z" : 10 }
{ "_id" : 23, "a" : { "b" : { "c" : { "e" : 3 } }, "y" : 6 }, "j" : { "k" : NumberLong(0), "p" : 9 }, "z" : 10 }
{ "_id" : 23, "a" : { "b" : { "c" : { "e" : 4 } }, "y" : 6 }, "j" : { "k" : NumberLong(1), "p" : 9 }, "z" : 10 }
```
### Query Test-32
```json
[ { "$match" : { "_id" : 24 } }, { "$unwind" : { "path" : "$a.b", "includeArrayIndex" : "j.k" } }, { "$project" : { "x" : 0 } } ]
```
### Results Test-32
```json
{ "_id" : 24, "a" : { "b" : 5, "y" : 6 }, "j" : { "k" : NumberLong(2) }, "z" : 14 }
{ "_id" : 24, "a" : { "b" : { "c" : { "e" : 3 } }, "y" : 6 }, "j" : { "k" : NumberLong(0) }, "z" : 14 }
{ "_id" : 24, "a" : { "b" : { "c" : { "e" : 4 } }, "y" : 6 }, "j" : { "k" : NumberLong(1) }, "z" : 14 }
```
### Query Test-33
```json
[ { "$match" : { "_id" : 13 } }, { "$unwind" : { "path" : "$a.b", "includeArrayIndex" : "a" } }, { "$project" : { "x" : 0 } } ]
```
### Results Test-33
```json
{ "_id" : 13, "a" : NumberLong(0), "z" : 7 }
{ "_id" : 13, "a" : NumberLong(1), "z" : 7 }
{ "_id" : 13, "a" : NumberLong(2), "z" : 7 }
```
### Query Test-34
```json
[ { "$match" : { "_id" : 16 } }, { "$unwind" : { "path" : "$a.b", "includeArrayIndex" : "a" } }, { "$project" : { "x" : 0 } } ]
```
### Results Test-34
```json
{ "_id" : 16, "a" : NumberLong(0), "z" : 12 }
{ "_id" : 16, "a" : NumberLong(1), "z" : 12 }
```
### Query Test-35
```json
[ { "$match" : { "_id" : 13 } }, { "$unwind" : { "path" : "$a.b", "includeArrayIndex" : "a.b" } }, { "$project" : { "x" : 0 } } ]
```
### Results Test-35
```json
{ "_id" : 13, "a" : { "b" : NumberLong(0), "y" : 7 }, "z" : 7 }
{ "_id" : 13, "a" : { "b" : NumberLong(1), "y" : 7 }, "z" : 7 }
{ "_id" : 13, "a" : { "b" : NumberLong(2), "y" : 7 }, "z" : 7 }
```
### Query Test-36
```json
[ { "$match" : { "_id" : 13 } }, { "$unwind" : { "path" : "$a.b", "includeArrayIndex" : "a.b.c" } }, { "$project" : { "x" : 0 } } ]
```
### Results Test-36
```json
{ "_id" : 13, "a" : { "b" : { "c" : NumberLong(0), "p" : 4 }, "y" : 7 }, "z" : 7 }
{ "_id" : 13, "a" : { "b" : { "c" : NumberLong(1) }, "y" : 7 }, "z" : 7 }
{ "_id" : 13, "a" : { "b" : { "c" : NumberLong(2) }, "y" : 7 }, "z" : 7 }
```
### Query Test-37
```json
[ { "$match" : { "_id" : 16 } }, { "$unwind" : { "path" : "$a.b", "includeArrayIndex" : "a.b.c" } }, { "$project" : { "x" : 0 } } ]
```
### Results Test-37
```json
{ "_id" : 16, "a" : { "b" : { "c" : NumberLong(0), "p" : 6 }, "y" : 11 }, "z" : 12 }
{ "_id" : 16, "a" : { "b" : { "c" : NumberLong(1), "p" : 10 }, "y" : 11 }, "z" : 12 }
```
### Query Test-38
```json
[ { "$match" : { "_id" : 13 } }, { "$unwind" : { "path" : "$a.b", "includeArrayIndex" : "a.b.c.d" } }, { "$project" : { "x" : 0 } } ]
```
### Results Test-38
```json
{ "_id" : 13, "a" : { "b" : { "c" : { "d" : NumberLong(0), "e" : 3 }, "p" : 4 }, "y" : 7 }, "z" : 7 }
{ "_id" : 13, "a" : { "b" : { "c" : { "d" : NumberLong(1), "e" : 5 } }, "y" : 7 }, "z" : 7 }
{ "_id" : 13, "a" : { "b" : { "c" : { "d" : NumberLong(2) } }, "y" : 7 }, "z" : 7 }
```
### Query Test-39
```json
[ { "$match" : { "_id" : 14 } }, { "$unwind" : { "path" : "$a.b", "includeArrayIndex" : "a.b.c.d" } }, { "$project" : { "x" : 0 } } ]
```
### Results Test-39
```json
{ "_id" : 14, "a" : { "b" : { "c" : { "d" : NumberLong(0), "e" : 4 }, "p" : 5 }, "y" : 10 }, "z" : 11 }
{ "_id" : 14, "a" : { "b" : { "c" : { "d" : NumberLong(1), "e" : 7 }, "p" : 8 }, "y" : 10 }, "z" : 11 }
{ "_id" : 14, "a" : { "b" : { "c" : { "d" : NumberLong(2) } }, "y" : 10 }, "z" : 11 }
```
### Query Test-40
```json
[ { "$match" : { "_id" : 15 } }, { "$unwind" : { "path" : "$a.b", "includeArrayIndex" : "a.b.c.d" } }, { "$project" : { "x" : 0 } } ]
```
### Results Test-40
```json
{ "_id" : 15, "a" : { "b" : { "c" : { "d" : NumberLong(0), "e" : 5 }, "p" : 6 }, "y" : 11 }, "z" : 12 }
{ "_id" : 15, "a" : { "b" : { "c" : { "d" : NumberLong(1), "e" : 9 }, "p" : 10 }, "y" : 11 }, "z" : 12 }
```
### Query Test-41
```json
[ { "$match" : { "_id" : 16 } }, { "$unwind" : { "path" : "$a.b", "includeArrayIndex" : "a.b.c.d" } }, { "$project" : { "x" : 0 } } ]
```
### Results Test-41
```json
{ "_id" : 16, "a" : { "b" : { "c" : { "d" : NumberLong(0) }, "p" : 6 }, "y" : 11 }, "z" : 12 }
{ "_id" : 16, "a" : { "b" : { "c" : { "d" : NumberLong(1) }, "p" : 10 }, "y" : 11 }, "z" : 12 }
```
### Query Test-42
```json
[ { "$match" : { "_id" : 17 } }, { "$unwind" : { "path" : "$a.b", "includeArrayIndex" : "a.b.c.d" } }, { "$project" : { "x" : 0 } } ]
```
### Results Test-42
```json
{ "_id" : 17, "a" : { "b" : { "c" : { "d" : NumberLong(0) }, "p" : 8 }, "y" : 15 }, "z" : 16 }
{ "_id" : 17, "a" : { "b" : { "c" : { "d" : NumberLong(1) }, "p" : 14 }, "y" : 15 }, "z" : 16 }
```
### Query Test-43
```json
[ { "$match" : { "_id" : 18 } }, { "$unwind" : { "path" : "$a.b", "includeArrayIndex" : "a.b.c.d" } }, { "$project" : { "x" : 0 } } ]
```
### Results Test-43
```json
{ "_id" : 18, "a" : { "b" : { "c" : { "d" : NumberLong(0) }, "p" : 10 }, "y" : 19 }, "z" : 20 }
{ "_id" : 18, "a" : { "b" : { "c" : { "d" : NumberLong(1) }, "p" : 18 }, "y" : 19 }, "z" : 20 }
```
### Query Test-44
```json
[ { "$match" : { "_id" : 25 } }, { "$unwind" : { "path" : "$a.b.c", "includeArrayIndex" : "a.b" } }, { "$project" : { "x" : 0 } } ]
```
### Results Test-44
```json
{ "_id" : 25, "a" : { "b" : NumberLong(0) } }
{ "_id" : 25, "a" : { "b" : NumberLong(1) } }
{ "_id" : 25, "a" : { "b" : NumberLong(2) } }
```
