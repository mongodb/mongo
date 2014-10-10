package progress

import (
	"bytes"
	. "github.com/smartystreets/goconvey/convey"
	"strings"
	"testing"
	"time"
)

func TestBasicProgressBar(t *testing.T) {
	var localCounter int
	writeBuffer := &bytes.Buffer{}

	Convey("With a simple ProgressBar", t, func() {
		pbar := &ProgressBar{
			Name:       "\nTEST",
			Max:        10,
			CounterPtr: &localCounter,
			WaitTime:   3 * time.Millisecond,
			Writer:     writeBuffer,
			BarLength:  10,
		}

		Convey("running it while incrementing its counter", func() {
			pbar.Start()
			// iterate though each value 1-10, sleeping to make sure it is written
			for localCounter = 0; localCounter < 10; localCounter++ {
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

func TestBarConcurrency(t *testing.T) {
	var localCounter int
	writeBuffer := &bytes.Buffer{}

	Convey("With a simple ProgressBar", t, func() {
		localCounter = 777
		pbar := &ProgressBar{
			Name:       "\nTEST",
			Max:        1000,
			CounterPtr: &localCounter,
			WaitTime:   10 * time.Millisecond,
			Writer:     writeBuffer,
		}

		Convey("starting and stopping it using some sketchy timing", func() {
			pbar.Start()
			time.Sleep(15 * time.Millisecond)
			pbar.Stop()
			// change this value after stopping and make sure it never gets used
			localCounter = 219
			time.Sleep(15 * time.Millisecond)

			Convey("the bar should have only logged one count", func() {
				results := writeBuffer.String()
				So(results, ShouldContainSubstring, "777")
				So(results, ShouldNotContainSubstring, "219")

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
