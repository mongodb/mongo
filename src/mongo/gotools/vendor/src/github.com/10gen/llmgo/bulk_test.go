// Copyright (C) MongoDB, Inc. 2015-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0
//
// Based on gopkg.io/mgo.v2 by Gustavo Niemeyer.
// See THIRD-PARTY-NOTICES for original license terms.

package mgo_test

import (
	. "gopkg.in/check.v1"
	"github.com/10gen/llmgo"
)

func (s *S) TestBulkInsert(c *C) {
	session, err := mgo.Dial("localhost:40001")
	c.Assert(err, IsNil)
	defer session.Close()

	coll := session.DB("mydb").C("mycoll")
	bulk := coll.Bulk()
	bulk.Insert(M{"n": 1})
	bulk.Insert(M{"n": 2}, M{"n": 3})
	r, err := bulk.Run()
	c.Assert(err, IsNil)
	c.Assert(r, FitsTypeOf, &mgo.BulkResult{})

	type doc struct{ N int }
	var res []doc
	err = coll.Find(nil).Sort("n").All(&res)
	c.Assert(err, IsNil)
	c.Assert(res, DeepEquals, []doc{{1}, {2}, {3}})
}

func (s *S) TestBulkInsertError(c *C) {
	session, err := mgo.Dial("localhost:40001")
	c.Assert(err, IsNil)
	defer session.Close()

	coll := session.DB("mydb").C("mycoll")
	bulk := coll.Bulk()
	bulk.Insert(M{"_id": 1}, M{"_id": 2}, M{"_id": 2}, M{"_id": 3})
	_, err = bulk.Run()
	c.Assert(err, ErrorMatches, ".*duplicate key.*")
	c.Assert(mgo.IsDup(err), Equals, true)

	type doc struct {
		N int `_id`
	}
	var res []doc
	err = coll.Find(nil).Sort("_id").All(&res)
	c.Assert(err, IsNil)
	c.Assert(res, DeepEquals, []doc{{1}, {2}})
}

func (s *S) TestBulkInsertErrorUnordered(c *C) {
	session, err := mgo.Dial("localhost:40001")
	c.Assert(err, IsNil)
	defer session.Close()

	coll := session.DB("mydb").C("mycoll")
	bulk := coll.Bulk()
	bulk.Unordered()
	bulk.Insert(M{"_id": 1}, M{"_id": 2}, M{"_id": 2}, M{"_id": 3})
	_, err = bulk.Run()
	c.Assert(err, ErrorMatches, ".*duplicate key.*")

	type doc struct {
		N int `_id`
	}
	var res []doc
	err = coll.Find(nil).Sort("_id").All(&res)
	c.Assert(err, IsNil)
	c.Assert(res, DeepEquals, []doc{{1}, {2}, {3}})
}

func (s *S) TestBulkInsertErrorUnorderedSplitBatch(c *C) {
	// The server has a batch limit of 1000 documents when using write commands.
	// This artificial limit did not exist with the old wire protocol, so to
	// avoid compatibility issues the implementation internally split batches
	// into the proper size and delivers them one by one. This test ensures that
	// the behavior of unordered (that is, continue on error) remains correct
	// when errors happen and there are batches left.
	session, err := mgo.Dial("localhost:40001")
	c.Assert(err, IsNil)
	defer session.Close()

	coll := session.DB("mydb").C("mycoll")
	bulk := coll.Bulk()
	bulk.Unordered()

	const total = 4096
	type doc struct {
		Id int `_id`
	}
	docs := make([]interface{}, total)
	for i := 0; i < total; i++ {
		docs[i] = doc{i}
	}
	docs[1] = doc{0}
	bulk.Insert(docs...)
	_, err = bulk.Run()
	c.Assert(err, ErrorMatches, ".*duplicate key.*")

	n, err := coll.Count()
	c.Assert(err, IsNil)
	c.Assert(n, Equals, total-1)

	var res doc
	err = coll.FindId(1500).One(&res)
	c.Assert(err, IsNil)
	c.Assert(res.Id, Equals, 1500)
}

func (s *S) TestBulkError(c *C) {
	session, err := mgo.Dial("localhost:40001")
	c.Assert(err, IsNil)
	defer session.Close()

	coll := session.DB("mydb").C("mycoll")

	// If it's just the same string multiple times, join it into a single message.
	bulk := coll.Bulk()
	bulk.Unordered()
	bulk.Insert(M{"_id": 1}, M{"_id": 2}, M{"_id": 2})
	_, err = bulk.Run()
	c.Assert(err, ErrorMatches, ".*duplicate key.*")
	c.Assert(err, Not(ErrorMatches), ".*duplicate key.*duplicate key")
	c.Assert(mgo.IsDup(err), Equals, true)

	// With matching errors but different messages, present them all.
	bulk = coll.Bulk()
	bulk.Unordered()
	bulk.Insert(M{"_id": "dupone"}, M{"_id": "dupone"}, M{"_id": "duptwo"}, M{"_id": "duptwo"})
	_, err = bulk.Run()
	if s.versionAtLeast(2, 6) {
		c.Assert(err, ErrorMatches, "multiple errors in bulk operation:\n(  - .*duplicate.*\n){2}$")
		c.Assert(err, ErrorMatches, "(?s).*dupone.*")
		c.Assert(err, ErrorMatches, "(?s).*duptwo.*")
	} else {
		// Wire protocol query doesn't return all errors.
		c.Assert(err, ErrorMatches, ".*duplicate.*")
	}
	c.Assert(mgo.IsDup(err), Equals, true)

	// With mixed errors, present them all.
	bulk = coll.Bulk()
	bulk.Unordered()
	bulk.Insert(M{"_id": 1}, M{"_id": []int{2}})
	_, err = bulk.Run()
	if s.versionAtLeast(2, 6) {
		c.Assert(err, ErrorMatches, "multiple errors in bulk operation:\n  - .*duplicate.*\n  - .*array.*\n$")
	} else {
		// Wire protocol query doesn't return all errors.
		c.Assert(err, ErrorMatches, ".*array.*")
	}
	c.Assert(mgo.IsDup(err), Equals, false)
}

func (s *S) TestBulkUpdate(c *C) {
	session, err := mgo.Dial("localhost:40001")
	c.Assert(err, IsNil)
	defer session.Close()

	coll := session.DB("mydb").C("mycoll")

	err = coll.Insert(M{"n": 1}, M{"n": 2}, M{"n": 3})
	c.Assert(err, IsNil)

	bulk := coll.Bulk()
	bulk.Update(M{"n": 1}, M{"$set": M{"n": 1}})
	bulk.Update(M{"n": 2}, M{"$set": M{"n": 20}})
	bulk.Update(M{"n": 5}, M{"$set": M{"n": 50}}) // Won't match.
	bulk.Update(M{"n": 1}, M{"$set": M{"n": 10}}, M{"n": 3}, M{"$set": M{"n": 30}})
	r, err := bulk.Run()
	c.Assert(err, IsNil)
	c.Assert(r.Matched, Equals, 4)
	if s.versionAtLeast(2, 6) {
		c.Assert(r.Modified, Equals, 3)
	}

	type doc struct{ N int }
	var res []doc
	err = coll.Find(nil).Sort("n").All(&res)
	c.Assert(err, IsNil)
	c.Assert(res, DeepEquals, []doc{{10}, {20}, {30}})
}

func (s *S) TestBulkUpdateError(c *C) {
	session, err := mgo.Dial("localhost:40001")
	c.Assert(err, IsNil)
	defer session.Close()

	coll := session.DB("mydb").C("mycoll")

	err = coll.Insert(M{"n": 1}, M{"n": 2}, M{"n": 3})
	c.Assert(err, IsNil)

	bulk := coll.Bulk()
	bulk.Update(
		M{"n": 1}, M{"$set": M{"n": 10}},
		M{"n": 2}, M{"$set": M{"n": 20, "_id": 20}},
		M{"n": 3}, M{"$set": M{"n": 30}},
	)
	r, err := bulk.Run()
	c.Assert(err, ErrorMatches, ".*_id.*")
	c.Assert(r, FitsTypeOf, &mgo.BulkResult{})

	type doc struct{ N int }
	var res []doc
	err = coll.Find(nil).Sort("n").All(&res)
	c.Assert(err, IsNil)
	c.Assert(res, DeepEquals, []doc{{2}, {3}, {10}})
}

func (s *S) TestBulkUpdateErrorUnordered(c *C) {
	session, err := mgo.Dial("localhost:40001")
	c.Assert(err, IsNil)
	defer session.Close()

	coll := session.DB("mydb").C("mycoll")

	err = coll.Insert(M{"n": 1}, M{"n": 2}, M{"n": 3})
	c.Assert(err, IsNil)

	bulk := coll.Bulk()
	bulk.Unordered()
	bulk.Update(
		M{"n": 1}, M{"$set": M{"n": 10}},
		M{"n": 2}, M{"$set": M{"n": 20, "_id": 20}},
		M{"n": 3}, M{"$set": M{"n": 30}},
	)
	r, err := bulk.Run()
	c.Assert(err, ErrorMatches, ".*_id.*")
	c.Assert(r, FitsTypeOf, &mgo.BulkResult{})

	type doc struct{ N int }
	var res []doc
	err = coll.Find(nil).Sort("n").All(&res)
	c.Assert(err, IsNil)
	c.Assert(res, DeepEquals, []doc{{2}, {10}, {30}})
}

func (s *S) TestBulkUpdateAll(c *C) {
	session, err := mgo.Dial("localhost:40001")
	c.Assert(err, IsNil)
	defer session.Close()

	coll := session.DB("mydb").C("mycoll")

	err = coll.Insert(M{"n": 1}, M{"n": 2}, M{"n": 3})
	c.Assert(err, IsNil)

	bulk := coll.Bulk()
	bulk.UpdateAll(M{"n": 1}, M{"$set": M{"n": 10}})
	bulk.UpdateAll(M{"n": 2}, M{"$set": M{"n": 2}})
	bulk.UpdateAll(M{"n": 5}, M{"$set": M{"n": 50}}) // Won't match.
	bulk.UpdateAll(M{}, M{"$inc": M{"n": 1}}, M{"n": 11}, M{"$set": M{"n": 5}})
	r, err := bulk.Run()
	c.Assert(err, IsNil)
	c.Assert(r.Matched, Equals, 6)
	if s.versionAtLeast(2, 6) {
		c.Assert(r.Modified, Equals, 5)
	}

	type doc struct{ N int }
	var res []doc
	err = coll.Find(nil).Sort("n").All(&res)
	c.Assert(err, IsNil)
	c.Assert(res, DeepEquals, []doc{{3}, {4}, {5}})
}

func (s *S) TestBulkMixedUnordered(c *C) {
	session, err := mgo.Dial("localhost:40001")
	c.Assert(err, IsNil)
	defer session.Close()

	coll := session.DB("mydb").C("mycoll")

	// Abuse undefined behavior to ensure the desired implementation is in place.
	bulk := coll.Bulk()
	bulk.Unordered()
	bulk.Insert(M{"n": 1})
	bulk.Update(M{"n": 2}, M{"$inc": M{"n": 1}})
	bulk.Insert(M{"n": 2})
	bulk.Update(M{"n": 3}, M{"$inc": M{"n": 1}})
	bulk.Update(M{"n": 1}, M{"$inc": M{"n": 1}})
	bulk.Insert(M{"n": 3})
	r, err := bulk.Run()
	c.Assert(err, IsNil)
	c.Assert(r.Matched, Equals, 3)
	if s.versionAtLeast(2, 6) {
		c.Assert(r.Modified, Equals, 3)
	}

	type doc struct{ N int }
	var res []doc
	err = coll.Find(nil).Sort("n").All(&res)
	c.Assert(err, IsNil)
	c.Assert(res, DeepEquals, []doc{{2}, {3}, {4}})
}

func (s *S) TestBulkUpsert(c *C) {
	session, err := mgo.Dial("localhost:40001")
	c.Assert(err, IsNil)
	defer session.Close()

	coll := session.DB("mydb").C("mycoll")

	err = coll.Insert(M{"n": 1}, M{"n": 2}, M{"n": 3})
	c.Assert(err, IsNil)

	bulk := coll.Bulk()
	bulk.Upsert(M{"n": 2}, M{"$set": M{"n": 20}})
	bulk.Upsert(M{"n": 4}, M{"$set": M{"n": 40}}, M{"n": 3}, M{"$set": M{"n": 30}})
	r, err := bulk.Run()
	c.Assert(err, IsNil)
	c.Assert(r, FitsTypeOf, &mgo.BulkResult{})

	type doc struct{ N int }
	var res []doc
	err = coll.Find(nil).Sort("n").All(&res)
	c.Assert(err, IsNil)
	c.Assert(res, DeepEquals, []doc{{1}, {20}, {30}, {40}})
}

func (s *S) TestBulkRemove(c *C) {
	session, err := mgo.Dial("localhost:40001")
	c.Assert(err, IsNil)
	defer session.Close()

	coll := session.DB("mydb").C("mycoll")

	err = coll.Insert(M{"n": 1}, M{"n": 2}, M{"n": 3}, M{"n": 4}, M{"n": 4})
	c.Assert(err, IsNil)

	bulk := coll.Bulk()
	bulk.Remove(M{"n": 1})
	bulk.Remove(M{"n": 2}, M{"n": 4})
	r, err := bulk.Run()
	c.Assert(err, IsNil)
	c.Assert(r.Matched, Equals, 3)

	type doc struct{ N int }
	var res []doc
	err = coll.Find(nil).Sort("n").All(&res)
	c.Assert(err, IsNil)
	c.Assert(res, DeepEquals, []doc{{3}, {4}})
}

func (s *S) TestBulkRemoveAll(c *C) {
	session, err := mgo.Dial("localhost:40001")
	c.Assert(err, IsNil)
	defer session.Close()

	coll := session.DB("mydb").C("mycoll")

	err = coll.Insert(M{"n": 1}, M{"n": 2}, M{"n": 3}, M{"n": 4}, M{"n": 4})
	c.Assert(err, IsNil)

	bulk := coll.Bulk()
	bulk.RemoveAll(M{"n": 1})
	bulk.RemoveAll(M{"n": 2}, M{"n": 4})
	r, err := bulk.Run()
	c.Assert(err, IsNil)
	c.Assert(r.Matched, Equals, 4)

	type doc struct{ N int }
	var res []doc
	err = coll.Find(nil).Sort("n").All(&res)
	c.Assert(err, IsNil)
	c.Assert(res, DeepEquals, []doc{{3}})
}

