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
				manager.Put(&Intent{DB: "2", C: "1", MetadataPath: "/4m/"})
				manager.Put(&Intent{DB: "1", C: "3", MetadataPath: "/3m/"})
				manager.Put(&Intent{DB: "1", C: "1", MetadataPath: "/1m/"})
				manager.Put(&Intent{DB: "1", C: "2", MetadataPath: "/2m/"})

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
							Intent{DB: "1", C: "1", Location: "/b1/", MetadataPath: "/1m/"})
						So(*it1, ShouldResemble,
							Intent{DB: "1", C: "2", Location: "/b2/", MetadataPath: "/2m/"})
						So(*it2, ShouldResemble,
							Intent{DB: "1", C: "3", Location: "/b3/", MetadataPath: "/3m/"})
						So(*it3, ShouldResemble,
							Intent{DB: "2", C: "1", Location: "/b4/", MetadataPath: "/4m/"})
					})
				})
			})

			Convey("but adding non-matching intents", func() {
				manager.Put(&Intent{DB: "7", C: "49", MetadataPath: "/5/"})
				manager.Put(&Intent{DB: "27", C: "B", MetadataPath: "/6/"})

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
