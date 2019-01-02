// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

// +build !race

// Disable race detector since these tests are inherently racy
package progress

import (
	"bytes"
	. "github.com/smartystreets/goconvey/convey"
	"strings"
	"testing"
	"time"
)

func TestBasicProgressBar(t *testing.T) {

	writeBuffer := &bytes.Buffer{}

	Convey("With a simple ProgressBar", t, func() {
		watching := NewCounter(10)
		pbar := &Bar{
			Name:      "\nTEST",
			Watching:  watching,
			WaitTime:  3 * time.Millisecond,
			Writer:    writeBuffer,
			BarLength: 10,
		}

		Convey("running it while incrementing its counter", func() {
			pbar.Start()
			// TODO make this test non-racy and reliable
			time.Sleep(10 * time.Millisecond)
			// iterate though each value 1-10, sleeping to make sure it is written
			for localCounter := 0; localCounter < 10; localCounter++ {
				watching.Inc(1)
				time.Sleep(5 * time.Millisecond)
			}
			pbar.Stop()

			Convey("the messages written in the buffer should cover all states", func() {
				results := writeBuffer.String()
				So(results, ShouldContainSubstring, "TEST")
				So(results, ShouldContainSubstring, BarLeft)
				So(results, ShouldContainSubstring, BarRight)
				So(results, ShouldContainSubstring, BarFilling)
				So(results, ShouldContainSubstring, BarEmpty)
				So(results, ShouldContainSubstring, "0/10")
				So(results, ShouldContainSubstring, "1/10")
				So(results, ShouldContainSubstring, "2/10")
				So(results, ShouldContainSubstring, "3/10")
				So(results, ShouldContainSubstring, "4/10")
				So(results, ShouldContainSubstring, "5/10")
				So(results, ShouldContainSubstring, "6/10")
				So(results, ShouldContainSubstring, "7/10")
				So(results, ShouldContainSubstring, "8/10")
				So(results, ShouldContainSubstring, "9/10")
				So(results, ShouldContainSubstring, "10.0%")
			})
		})
	})
}

func TestProgressBarWithNoMax(t *testing.T) {
	writeBuffer := &bytes.Buffer{}

	Convey("With a simple ProgressBar with no max value", t, func() {
		watching := NewCounter(0)
		watching.Inc(5)
		pbar := &Bar{
			Name:     "test",
			Watching: watching,
			Writer:   writeBuffer,
		}
		Convey("rendering the progress should still work, but not draw a bar", func() {
			pbar.renderToWriter()
			So(writeBuffer.String(), ShouldContainSubstring, "5")
			So(writeBuffer.String(), ShouldContainSubstring, "test")
			So(writeBuffer.String(), ShouldNotContainSubstring, "[")
			So(writeBuffer.String(), ShouldNotContainSubstring, "]")
		})
	})
}

func TestBarConcurrency(t *testing.T) {
	writeBuffer := &bytes.Buffer{}

	Convey("With a simple ProgressBar", t, func() {
		watching := NewCounter(1000)
		watching.Inc(777)
		pbar := &Bar{
			Name:     "\nTEST",
			Watching: watching,
			WaitTime: 10 * time.Millisecond,
			Writer:   writeBuffer,
		}

		Convey("if it rendered only once", func() {
			pbar.Start()
			time.Sleep(15 * time.Millisecond)
			watching.Inc(1)
			results := writeBuffer.String()
			So(results, ShouldContainSubstring, "777")
			So(results, ShouldNotContainSubstring, "778")

			Convey("it will render a second time on stop", func() {
				pbar.Stop()
				results := writeBuffer.String()
				So(results, ShouldContainSubstring, "777")
				So(results, ShouldContainSubstring, "778")

				Convey("and trying to start or stop the bar again should panic", func() {
					So(func() { pbar.Start() }, ShouldPanic)
					So(func() { pbar.Stop() }, ShouldPanic)
				})
			})
		})
	})
}

func TestBarDrawing(t *testing.T) {
	Convey("Drawing some test bars and checking their character counts", t, func() {
		Convey("20 wide @ 50%", func() {
			b := drawBar(20, .5)
			So(strings.Count(b, BarFilling), ShouldEqual, 10)
			So(strings.Count(b, BarEmpty), ShouldEqual, 10)
			So(b, ShouldContainSubstring, BarLeft)
			So(b, ShouldContainSubstring, BarRight)
		})
		Convey("100 wide @ 50%", func() {
			b := drawBar(100, .5)
			So(strings.Count(b, BarFilling), ShouldEqual, 50)
			So(strings.Count(b, BarEmpty), ShouldEqual, 50)
		})
		Convey("100 wide @ 99.9999%", func() {
			b := drawBar(100, .999999)
			So(strings.Count(b, BarFilling), ShouldEqual, 99)
			So(strings.Count(b, BarEmpty), ShouldEqual, 1)
		})
		Convey("9 wide @ 72%", func() {
			b := drawBar(9, .72)
			So(strings.Count(b, BarFilling), ShouldEqual, 6)
			So(strings.Count(b, BarEmpty), ShouldEqual, 3)
		})
		Convey("10 wide @ 0%", func() {
			b := drawBar(10, 0)
			So(strings.Count(b, BarFilling), ShouldEqual, 0)
			So(strings.Count(b, BarEmpty), ShouldEqual, 10)
		})
		Convey("10 wide @ 100%", func() {
			b := drawBar(10, 1)
			So(strings.Count(b, BarFilling), ShouldEqual, 10)
			So(strings.Count(b, BarEmpty), ShouldEqual, 0)
		})
		Convey("10 wide @ -60%", func() {
			b := drawBar(10, -0.6)
			So(strings.Count(b, BarFilling), ShouldEqual, 0)
			So(strings.Count(b, BarEmpty), ShouldEqual, 10)
		})
		Convey("10 wide @ 160%", func() {
			b := drawBar(10, 1.6)
			So(strings.Count(b, BarFilling), ShouldEqual, 10)
			So(strings.Count(b, BarEmpty), ShouldEqual, 0)
		})
	})
}

func TestBarUnits(t *testing.T) {
	writeBuffer := &bytes.Buffer{}

	Convey("With a simple ProgressBar with IsBytes==true", t, func() {
		watching := NewCounter(1024 * 1024)
		watching.Inc(777)
		pbar := &Bar{
			Name:     "\nTEST",
			Watching: watching,
			WaitTime: 10 * time.Millisecond,
			Writer:   writeBuffer,
			IsBytes:  true,
		}

		Convey("the written output should contain units", func() {
			pbar.renderToWriter()
			So(writeBuffer.String(), ShouldContainSubstring, "B")
			So(writeBuffer.String(), ShouldContainSubstring, "MB")
		})
	})
}
