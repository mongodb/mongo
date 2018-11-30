// Copyright (C) MongoDB, Inc. 2015-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0
//
// Based on gopkg.io/mgo.v2 by Gustavo Niemeyer.
// See THIRD-PARTY-NOTICES for original license terms.

package mgo

import (
	. "gopkg.in/check.v1"
)

type QS struct{}

var _ = Suite(&QS{})

func (s *QS) TestSequentialGrowth(c *C) {
	q := queue{}
	n := 2048
	for i := 0; i != n; i++ {
		q.Push(i)
	}
	for i := 0; i != n; i++ {
		c.Assert(q.Pop(), Equals, i)
	}
}

var queueTestLists = [][]int{
	// {0, 1, 2, 3, 4, 5, 6, 7, 8, 9}
	{0, 1, 2, 3, 4, 5, 6, 7, 8, 9},

	// {8, 9, 10, 11, ... 2, 3, 4, 5, 6, 7}
	{0, 1, 2, 3, 4, 5, 6, 7, -1, -1, 8, 9, 10, 11},

	// {8, 9, 10, 11, ... 2, 3, 4, 5, 6, 7}
	{0, 1, 2, 3, -1, -1, 4, 5, 6, 7, 8, 9, 10, 11},

	// {0, 1, 2, 3, 4, 5, 6, 7, 8}
	{0, 1, 2, 3, 4, 5, 6, 7, 8,
		-1, -1, -1, -1, -1, -1, -1, -1, -1,
		0, 1, 2, 3, 4, 5, 6, 7, 8},
}

func (s *QS) TestQueueTestLists(c *C) {
	test := []int{}
	testi := 0
	reset := func() {
		test = test[0:0]
		testi = 0
	}
	push := func(i int) {
		test = append(test, i)
	}
	pop := func() (i int) {
		if testi == len(test) {
			return -1
		}
		i = test[testi]
		testi++
		return
	}

	for _, list := range queueTestLists {
		reset()
		q := queue{}
		for _, n := range list {
			if n == -1 {
				c.Assert(q.Pop(), Equals, pop(), Commentf("With list %#v", list))
			} else {
				q.Push(n)
				push(n)
			}
		}

		for n := pop(); n != -1; n = pop() {
			c.Assert(q.Pop(), Equals, n, Commentf("With list %#v", list))
		}

		c.Assert(q.Pop(), Equals, nil, Commentf("With list %#v", list))
	}
}
