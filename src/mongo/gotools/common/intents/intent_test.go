// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package intents

import (
	"github.com/mongodb/mongo-tools/common/testutil"
	. "github.com/smartystreets/goconvey/convey"
	"testing"
)

func TestIntentManager(t *testing.T) {
	var manager *Manager

	testutil.VerifyTestType(t, testutil.UnitTestType)

	Convey("With an empty IntentManager", t, func() {
		manager = NewIntentManager()
		So(manager, ShouldNotBeNil)

		Convey("putting  a series of added bson intents", func() {
			manager.Put(&Intent{DB: "1", C: "1", Location: "/b1/"})
			manager.Put(&Intent{DB: "1", C: "2", Location: "/b2/"})
			manager.Put(&Intent{DB: "1", C: "3", Location: "/b3/"})
			manager.Put(&Intent{DB: "2", C: "1", Location: "/b4/"})
			So(len(manager.intentsByDiscoveryOrder), ShouldEqual, 4)
			So(len(manager.intents), ShouldEqual, 4)

			Convey("and then some matching metadata intents", func() {
				manager.Put(&Intent{DB: "2", C: "1", MetadataLocation: "/4m/"})
				manager.Put(&Intent{DB: "1", C: "3", MetadataLocation: "/3m/"})
				manager.Put(&Intent{DB: "1", C: "1", MetadataLocation: "/1m/"})
				manager.Put(&Intent{DB: "1", C: "2", MetadataLocation: "/2m/"})

				Convey("the size of the queue should be unchanged", func() {
					So(len(manager.intentsByDiscoveryOrder), ShouldEqual, 4)
					So(len(manager.intents), ShouldEqual, 4)
				})

				Convey("popping them from the IntentManager", func() {
					manager.Finalize(Legacy)
					it0 := manager.Pop()
					it1 := manager.Pop()
					it2 := manager.Pop()
					it3 := manager.Pop()
					it4 := manager.Pop()
					So(it4, ShouldBeNil)

					Convey("should return them in insert order", func() {
						So(*it0, ShouldResemble,
							Intent{DB: "1", C: "1", Location: "/b1/", MetadataLocation: "/1m/"})
						So(*it1, ShouldResemble,
							Intent{DB: "1", C: "2", Location: "/b2/", MetadataLocation: "/2m/"})
						So(*it2, ShouldResemble,
							Intent{DB: "1", C: "3", Location: "/b3/", MetadataLocation: "/3m/"})
						So(*it3, ShouldResemble,
							Intent{DB: "2", C: "1", Location: "/b4/", MetadataLocation: "/4m/"})
					})
				})
			})

			Convey("but adding non-matching intents", func() {
				manager.Put(&Intent{DB: "7", C: "49", MetadataLocation: "/5/"})
				manager.Put(&Intent{DB: "27", C: "B", MetadataLocation: "/6/"})

				Convey("should increase the size, because they are not merged in", func() {
					So(len(manager.intentsByDiscoveryOrder), ShouldEqual, 6)
					So(len(manager.intents), ShouldEqual, 6)
				})
			})

			Convey("using the Peek() method", func() {
				peeked := manager.Peek()
				So(peeked, ShouldNotBeNil)
				So(peeked, ShouldResemble, manager.intentsByDiscoveryOrder[0])

				Convey("modifying the returned copy should not modify the original", func() {
					peeked.DB = "SHINY NEW VALUE"
					So(peeked, ShouldNotResemble, manager.intentsByDiscoveryOrder[0])
				})
			})
		})
	})
}
