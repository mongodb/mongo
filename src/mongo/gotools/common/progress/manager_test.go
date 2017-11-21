// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package progress

import (
	"bytes"
	. "github.com/smartystreets/goconvey/convey"
	"strconv"
	"strings"
	"sync"
	"testing"
	"time"
)

type safeBuffer struct {
	sync.Mutex
	bytes.Buffer
}

func (b *safeBuffer) Write(p []byte) (n int, err error) {
	b.Lock()
	defer b.Unlock()
	return b.Buffer.Write(p)
}

func (b *safeBuffer) String() string {
	b.Lock()
	defer b.Unlock()
	return b.Buffer.String()
}

func (b *safeBuffer) Reset() {
	b.Lock()
	defer b.Unlock()
	b.Buffer.Reset()
}

func TestManagerAttachAndDetach(t *testing.T) {
	writeBuffer := new(safeBuffer)
	var manager *BarWriter

	Convey("With an empty progress.BarWriter", t, func() {
		manager = NewBarWriter(writeBuffer, time.Second, 10, false)
		So(manager, ShouldNotBeNil)

		Convey("adding 3 bars", func() {
			progressor := NewCounter(10)
			progressor.Inc(5)
			manager.Attach("TEST1", progressor)
			manager.Attach("TEST2", progressor)
			manager.Attach("TEST3", progressor)

			So(len(manager.bars), ShouldEqual, 3)

			Convey("should write all three bars ar once", func() {
				manager.renderAllBars()
				writtenString := writeBuffer.String()
				So(writtenString, ShouldContainSubstring, "TEST1")
				So(writtenString, ShouldContainSubstring, "TEST2")
				So(writtenString, ShouldContainSubstring, "TEST3")
			})

			Convey("detaching the second bar", func() {
				manager.Detach("TEST2")
				So(len(manager.bars), ShouldEqual, 2)

				Convey("should print 1,3", func() {
					manager.renderAllBars()
					writtenString := writeBuffer.String()
					So(writtenString, ShouldContainSubstring, "TEST1")
					So(writtenString, ShouldNotContainSubstring, "TEST2")
					So(writtenString, ShouldContainSubstring, "TEST3")
					So(
						strings.Index(writtenString, "TEST1"),
						ShouldBeLessThan,
						strings.Index(writtenString, "TEST3"),
					)
				})

				Convey("but adding a new bar should print 1,2,4", func() {
					manager.Attach("TEST4", progressor)

					So(len(manager.bars), ShouldEqual, 3)
					manager.renderAllBars()
					writtenString := writeBuffer.String()
					So(writtenString, ShouldContainSubstring, "TEST1")
					So(writtenString, ShouldNotContainSubstring, "TEST2")
					So(writtenString, ShouldContainSubstring, "TEST3")
					So(writtenString, ShouldContainSubstring, "TEST4")
					So(
						strings.Index(writtenString, "TEST1"),
						ShouldBeLessThan,
						strings.Index(writtenString, "TEST3"),
					)
					So(
						strings.Index(writtenString, "TEST3"),
						ShouldBeLessThan,
						strings.Index(writtenString, "TEST4"),
					)
				})
				Reset(func() { writeBuffer.Reset() })

			})
			Reset(func() { writeBuffer.Reset() })
		})
	})
}

func TestManagerStartAndStop(t *testing.T) {
	writeBuffer := new(safeBuffer)
	var manager *BarWriter

	Convey("With a progress.BarWriter with a waitTime of 10 ms and one bar", t, func() {
		manager = NewBarWriter(writeBuffer, time.Millisecond*10, 10, false)
		So(manager, ShouldNotBeNil)
		watching := NewCounter(10)
		watching.Inc(5)
		manager.Attach("TEST", watching)

		So(manager.waitTime, ShouldEqual, time.Millisecond*10)
		So(len(manager.bars), ShouldEqual, 1)

		Convey("running the manager for 45 ms and stopping", func() {
			manager.Start()
			time.Sleep(time.Millisecond * 45) // enough time for the manager to write 4 times
			manager.Stop()

			Convey("should generate 4 writes of the bar", func() {
				output := writeBuffer.String()
				So(strings.Count(output, "TEST"), ShouldEqual, 4)
			})

			Convey("starting and stopping the manager again should not panic", func() {
				So(manager.Start, ShouldNotPanic)
				So(manager.Stop, ShouldNotPanic)
			})
		})
	})
}

func TestNumberOfWrites(t *testing.T) {
	var cw *CountWriter
	var manager *BarWriter
	Convey("With a test manager and counting writer", t, func() {
		cw = new(CountWriter)
		manager = NewBarWriter(cw, time.Millisecond*10, 10, false)
		So(manager, ShouldNotBeNil)

		manager.Attach("1", NewCounter(10))

		Convey("with one attached bar", func() {
			So(len(manager.bars), ShouldEqual, 1)

			Convey("only one write should be made per render", func() {
				manager.renderAllBars()
				So(cw.Count(), ShouldEqual, 1)
			})
		})

		Convey("with two bars attached", func() {
			manager.Attach("2", NewCounter(10))
			So(len(manager.bars), ShouldEqual, 2)

			Convey("three writes should be made per render, since an empty write is added", func() {
				manager.renderAllBars()
				So(cw.Count(), ShouldEqual, 3)
			})
		})

		Convey("with 57 bars attached", func() {
			for i := 2; i <= 57; i++ {
				manager.Attach(strconv.Itoa(i), NewCounter(10))
			}
			So(len(manager.bars), ShouldEqual, 57)

			Convey("58 writes should be made per render, since an empty write is added", func() {
				manager.renderAllBars()
				So(cw.Count(), ShouldEqual, 58)
			})
		})
	})
}

// helper type for counting calls to a writer
type CountWriter int

func (cw CountWriter) Count() int {
	return int(cw)
}

func (cw *CountWriter) Write(b []byte) (int, error) {
	*cw++
	return len(b), nil
}
