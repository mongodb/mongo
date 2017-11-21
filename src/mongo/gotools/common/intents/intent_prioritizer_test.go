// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package intents

import (
	"container/heap"
	"testing"

	"github.com/mongodb/mongo-tools/common/testutil"
	. "github.com/smartystreets/goconvey/convey"
	"gopkg.in/mgo.v2/bson"
)

func TestLegacyPrioritizer(t *testing.T) {

	testutil.VerifyTestType(t, testutil.UnitTestType)

	Convey("With a legacyPrioritizer initialized with an ordered intent list", t, func() {
		testList := []*Intent{
			&Intent{DB: "1"},
			&Intent{DB: "2"},
			&Intent{DB: "3"},
		}
		legacy := NewLegacyPrioritizer(testList)
		So(legacy, ShouldNotBeNil)

		Convey("the priority should be defined by 'first-in-first-out'", func() {
			it0 := legacy.Get()
			it1 := legacy.Get()
			it2 := legacy.Get()
			it3 := legacy.Get()
			So(it3, ShouldBeNil)
			So(it0.DB, ShouldBeLessThan, it1.DB)
			So(it1.DB, ShouldBeLessThan, it2.DB)
		})
	})
}

func TestBasicDBHeapBehavior(t *testing.T) {
	var dbheap heap.Interface

	testutil.VerifyTestType(t, testutil.UnitTestType)

	Convey("With an empty dbHeap", t, func() {
		dbheap = &DBHeap{}
		heap.Init(dbheap)

		Convey("when inserting unordered dbCounters with different active counts", func() {
			heap.Push(dbheap, &dbCounter{75, nil})
			heap.Push(dbheap, &dbCounter{121, nil})
			heap.Push(dbheap, &dbCounter{76, nil})
			heap.Push(dbheap, &dbCounter{51, nil})
			heap.Push(dbheap, &dbCounter{82, nil})
			heap.Push(dbheap, &dbCounter{117, nil})
			heap.Push(dbheap, &dbCounter{49, nil})
			heap.Push(dbheap, &dbCounter{101, nil})
			heap.Push(dbheap, &dbCounter{122, nil})
			heap.Push(dbheap, &dbCounter{33, nil})
			heap.Push(dbheap, &dbCounter{0, nil})

			Convey("they should pop in active order, least to greatest", func() {
				prev := -1
				for dbheap.Len() > 0 {
					popped := heap.Pop(dbheap).(*dbCounter)
					So(popped.active, ShouldBeGreaterThan, prev)
					prev = popped.active
				}
			})
		})

		Convey("when inserting unordered dbCounters with different bson sizes", func() {
			heap.Push(dbheap, &dbCounter{0, []*Intent{&Intent{Size: 70}}})
			heap.Push(dbheap, &dbCounter{0, []*Intent{&Intent{Size: 1024}}})
			heap.Push(dbheap, &dbCounter{0, []*Intent{&Intent{Size: 97}}})
			heap.Push(dbheap, &dbCounter{0, []*Intent{&Intent{Size: 3}}})
			heap.Push(dbheap, &dbCounter{0, []*Intent{&Intent{Size: 1024 * 1024}}})

			Convey("they should pop in bson size order, greatest to least", func() {
				prev := int64(1024*1024 + 1) // Maximum
				for dbheap.Len() > 0 {
					popped := heap.Pop(dbheap).(*dbCounter)
					So(popped.collections[0].Size, ShouldBeLessThan, prev)
					prev = popped.collections[0].Size
				}
			})
		})
	})
}

func TestDBCounterCollectionSorting(t *testing.T) {

	testutil.VerifyTestType(t, testutil.UnitTestType)

	Convey("With a dbCounter and an unordered collection of intents", t, func() {
		dbc := &dbCounter{
			collections: []*Intent{
				&Intent{Size: 100},
				&Intent{Size: 1000},
				&Intent{Size: 1},
				&Intent{Size: 10},
			},
		}

		Convey("popping the sorted intents should return in decreasing BSONSize", func() {
			dbc.SortCollectionsBySize()
			So(dbc.PopIntent().Size, ShouldEqual, 1000)
			So(dbc.PopIntent().Size, ShouldEqual, 100)
			So(dbc.PopIntent().Size, ShouldEqual, 10)
			So(dbc.PopIntent().Size, ShouldEqual, 1)
			So(dbc.PopIntent(), ShouldBeNil)
			So(dbc.PopIntent(), ShouldBeNil)
		})
	})
}

func TestBySizeAndView(t *testing.T) {
	var prioritizer IntentPrioritizer

	testutil.VerifyTestType(t, testutil.UnitTestType)

	Convey("With a prioritizer initialized with on a set of intents", t, func() {
		intents := []*Intent{
			&Intent{C: "non-view2", Size: 32},
			&Intent{C: "view", Size: 0,
				Options: &bson.D{{"viewOn", true}},
			},
			&Intent{C: "non-view1", Size: 1024},
			&Intent{C: "non-view3", Size: 2},
			&Intent{C: "view", Size: 0,
				Options: &bson.D{{"viewOn", true}},
			},
		}
		prioritizer = NewLongestTaskFirstPrioritizer(intents)
		Convey("getting the sorted intents should produce views first, followed by largest to smallest", func() {

			So(prioritizer.Get().C, ShouldEqual, "view")
			So(prioritizer.Get().C, ShouldEqual, "view")
			So(prioritizer.Get().C, ShouldEqual, "non-view1")
			So(prioritizer.Get().C, ShouldEqual, "non-view2")
			So(prioritizer.Get().C, ShouldEqual, "non-view3")
		})

	})

}

func TestSimulatedMultiDBJob(t *testing.T) {
	var prioritizer IntentPrioritizer

	testutil.VerifyTestType(t, testutil.UnitTestType)

	Convey("With a prioritizer initialized with a set of intents", t, func() {
		intents := []*Intent{
			&Intent{C: "small", DB: "db2", Size: 32},
			&Intent{C: "medium", DB: "db2", Size: 128},
			&Intent{C: "giant", DB: "db1", Size: 1024},
			&Intent{C: "tiny", DB: "db1", Size: 2},
		}
		prioritizer = NewMultiDatabaseLTFPrioritizer(intents)
		So(prioritizer, ShouldNotBeNil)

		Convey("and a running simulation of two jobs threads:", func() {
			Convey("the first two intents should be of different dbs", func() {
				i0 := prioritizer.Get()
				So(i0, ShouldNotBeNil)
				i1 := prioritizer.Get()
				So(i1, ShouldNotBeNil)

				Convey("the first intent should be the largest bson file", func() {
					So(i0.C, ShouldEqual, "giant")
					So(i0.DB, ShouldEqual, "db1")
				})

				Convey("the second intent should be the largest bson file of db2", func() {
					So(i1.C, ShouldEqual, "medium")
					So(i1.DB, ShouldEqual, "db2")
				})

				Convey("with the second job finishing the smaller intents", func() {
					prioritizer.Finish(i1)
					i2 := prioritizer.Get()
					So(i2, ShouldNotBeNil)
					prioritizer.Finish(i2)
					i3 := prioritizer.Get()
					So(i3, ShouldNotBeNil)

					Convey("the next job should be from db2", func() {
						So(i2.C, ShouldEqual, "small")
						So(i2.DB, ShouldEqual, "db2")
					})

					Convey("the final job should be from db1", func() {
						So(i3.C, ShouldEqual, "tiny")
						So(i3.DB, ShouldEqual, "db1")

						Convey("which means that there should be two active db1 jobs", func() {
							counter := prioritizer.(*multiDatabaseLTFPrioritizer).counterMap["db1"]
							So(counter.active, ShouldEqual, 2)
						})
					})

					Convey("the heap should now be empty", func() {
						So(prioritizer.(*multiDatabaseLTFPrioritizer).dbHeap.Len(), ShouldEqual, 0)
					})
				})
			})
		})
	})
}
