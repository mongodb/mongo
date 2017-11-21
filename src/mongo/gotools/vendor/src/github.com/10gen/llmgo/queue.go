// Copyright (C) MongoDB, Inc. 2015-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0
//
// Based on gopkg.io/mgo.v2 by Gustavo Niemeyer.
// See THIRD-PARTY-NOTICES for original license terms.

package mgo

type queue struct {
	elems               []interface{}
	nelems, popi, pushi int
}

func (q *queue) Len() int {
	return q.nelems
}

func (q *queue) Push(elem interface{}) {
	//debugf("Pushing(pushi=%d popi=%d cap=%d): %#v\n",
	//       q.pushi, q.popi, len(q.elems), elem)
	if q.nelems == len(q.elems) {
		q.expand()
	}
	q.elems[q.pushi] = elem
	q.nelems++
	q.pushi = (q.pushi + 1) % len(q.elems)
	//debugf(" Pushed(pushi=%d popi=%d cap=%d): %#v\n",
	//       q.pushi, q.popi, len(q.elems), elem)
}

func (q *queue) Pop() (elem interface{}) {
	//debugf("Popping(pushi=%d popi=%d cap=%d)\n",
	//       q.pushi, q.popi, len(q.elems))
	if q.nelems == 0 {
		return nil
	}
	elem = q.elems[q.popi]
	q.elems[q.popi] = nil // Help GC.
	q.nelems--
	q.popi = (q.popi + 1) % len(q.elems)
	//debugf(" Popped(pushi=%d popi=%d cap=%d): %#v\n",
	//       q.pushi, q.popi, len(q.elems), elem)
	return elem
}

func (q *queue) expand() {
	curcap := len(q.elems)
	var newcap int
	if curcap == 0 {
		newcap = 8
	} else if curcap < 1024 {
		newcap = curcap * 2
	} else {
		newcap = curcap + (curcap / 4)
	}
	elems := make([]interface{}, newcap)

	if q.popi == 0 {
		copy(elems, q.elems)
		q.pushi = curcap
	} else {
		newpopi := newcap - (curcap - q.popi)
		copy(elems, q.elems[:q.popi])
		copy(elems[newpopi:], q.elems[q.popi:])
		q.popi = newpopi
	}
	for i := range q.elems {
		q.elems[i] = nil // Help GC.
	}
	q.elems = elems
}
