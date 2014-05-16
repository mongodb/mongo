package txn

import (
	"fmt"
	"labix.org/v2/mgo/bson"
	. "launchpad.net/gocheck"
)

type TarjanSuite struct{}

var _ = Suite(TarjanSuite{})

func bid(n int) bson.ObjectId {
	return bson.ObjectId(fmt.Sprintf("%024d", n))
}

func bids(ns ...int) (ids []bson.ObjectId) {
	for _, n := range ns {
		ids = append(ids, bid(n))
	}
	return
}

func (TarjanSuite) TestExample(c *C) {
	successors := map[bson.ObjectId][]bson.ObjectId{
		bid(1): bids(2),
		bid(2): bids(1, 5),
		bid(3): bids(4),
		bid(4): bids(3, 5),
		bid(5): bids(6),
		bid(6): bids(7),
		bid(7): bids(8),
		bid(8): bids(6, 9),
		bid(9): bids(),
	}

	c.Assert(tarjanSort(successors), DeepEquals, [][]bson.ObjectId{
		bids(9),
		bids(8, 7, 6),
		bids(5),
		bids(2, 1),
		bids(4, 3),
	})
}
