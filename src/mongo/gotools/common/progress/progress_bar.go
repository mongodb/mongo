// Package progress exposes utilities to asynchronously monitor and display processing progress.
package progress

import (
	"bytes"
	"fmt"
	"github.com/mongodb/mongo-tools/common/text"
	"io"
	"sync/atomic"
	"time"
)

const (
	DefaultWaitTime = 3 * time.Second
	BarFilling      = "#"
	BarEmpty        = "."
	BarLeft         = "["
	BarRight        = "]"
)

// countProgressor is an implementation of Progressor that uses
type countProgressor struct {
	max     int64
	current int64
}

func (c *countProgressor) Progress() (int64, int64) {
	current := atomic.LoadInt64(&c.current)
	return c.max, current
}

func (c *countProgressor) Inc(amount int64) {
	atomic.AddInt64(&c.current, amount)
}

func (c *countProgressor) Set(amount int64) {
	atomic.StoreInt64(&c.current, amount)
}

func NewCounter(max int64) *countProgressor {
	return &countProgressor{max, 0}
}

// Progressor can be implemented to allow an object to hook up to a progress.Bar.
type Progressor interface {
	// Progress returns a pair of integers: the total amount to reach 100%, and
	// the amount completed. This method is called by progress.Bar to
	// determine what percentage to display.
	Progress() (int64, int64)
}

// Updateable is a Progressor which also exposes the ability for the progressing
// value to be incremented, or reset.
type Updateable interface {
	// Inc increments the current progress counter by the given amount.
	Inc(amount int64)

	// Set resets the progress counter to the given amount.
	Set(amount int64)

	Progressor
}

// Bar is a tool for concurrently monitoring the progress
// of a task with a simple linear ASCII visualization
type Bar struct {
	// Name is an identifier printed along with the bar
	Name string
	// BarLength is the number of characters used to print the bar
	BarLength int

	// IsBytes denotes whether byte-specific formatting (kB, MB, GB) should
	// be applied to the numeric output
	IsBytes bool

	// Watching is the object that implements the Progressor to expose the
	// values necessary for calculation
	Watching Progressor

	// Writer is where the Bar is written out to
	Writer io.Writer
	// WaitTime is the time to wait between writing the bar
	WaitTime time.Duration

	stopChan     chan struct{}
	stopChanSync chan struct{}

	// hasRendered indicates that the bar has been rendered at least once
	// and implies that when detaching should be rendered one more time
	hasRendered bool
}

// Start starts the Bar goroutine. Once Start is called, a bar will
// be written to the given Writer at regular intervals. The goroutine
// can only be stopped manually using the Stop() method. The Bar
// must be set up before calling this. Panics if Start has already been called.
func (pb *Bar) Start() {
	pb.validate()
	// we only check for the writer if we're using a single bar without a manager
	if pb.Writer == nil {
		panic("Cannot use a Bar with an unset Writer")
	}
	pb.stopChan = make(chan struct{})
	pb.stopChanSync = make(chan struct{})

	go pb.start()
}

// validate does a set of sanity checks against the progress bar, and panics
// if the bar is unfit for use
func (pb *Bar) validate() {
	if pb.Watching == nil {
		panic("Cannot use a Bar with a nil Watching")
	}
	if pb.stopChan != nil {
		panic("Cannot start a Bar more than once")
	}
}

// Stop kills the Bar goroutine, stopping it from writing.
// Generally called as
//  myBar.Start()
//  defer myBar.Stop()
// to stop leakage
// Stop() needs to be synchronous in order that when pb.Stop() is called
// all of the rendering has completed
func (pb *Bar) Stop() {
	close(pb.stopChan)
	<-pb.stopChanSync
}

func (pb *Bar) formatCounts() (string, string) {
	maxCount, currentCount := pb.Watching.Progress()
	if pb.IsBytes {
		return text.FormatByteAmount(maxCount), text.FormatByteAmount(currentCount)
	}
	return fmt.Sprintf("%v", maxCount), fmt.Sprintf("%v", currentCount)
}

// computes all necessary values renders to the bar's Writer
func (pb *Bar) renderToWriter() {
	pb.hasRendered = true
	maxCount, currentCount := pb.Watching.Progress()
	maxStr, currentStr := pb.formatCounts()
	if maxCount == 0 {
		// if we have no max amount, just print a count
		fmt.Fprintf(pb.Writer, "%v\t%v", pb.Name, currentStr)
		return
	}
	// otherwise, print a bar and percents
	percent := float64(currentCount) / float64(maxCount)
	fmt.Fprintf(pb.Writer, "%v %v\t%s/%s (%2.1f%%)",
		drawBar(pb.BarLength, percent),
		pb.Name,
		currentStr,
		maxStr,
		percent*100,
	)
}

func (pb *Bar) renderToGridRow(grid *text.GridWriter) {
	pb.hasRendered = true
	maxCount, currentCount := pb.Watching.Progress()
	maxStr, currentStr := pb.formatCounts()
	if maxCount == 0 {
		// if we have no max amount, just print a count
		grid.WriteCells(pb.Name, currentStr)
	} else {
		percent := float64(currentCount) / float64(maxCount)
		grid.WriteCells(
			drawBar(pb.BarLength, percent),
			pb.Name,
			fmt.Sprintf("%s/%s", currentStr, maxStr),
			fmt.Sprintf("(%2.1f%%)", percent*100),
		)
	}
	grid.EndRow()
}

// the main concurrent loop
func (pb *Bar) start() {
	if pb.WaitTime <= 0 {
		pb.WaitTime = DefaultWaitTime
	}
	ticker := time.NewTicker(pb.WaitTime)
	defer ticker.Stop()

	for {
		select {
		case <-pb.stopChan:
			if pb.hasRendered {
				// if we've rendered this bar at least once, render it one last time
				pb.renderToWriter()
			}
			close(pb.stopChanSync)
			return
		case <-ticker.C:
			pb.renderToWriter()
		}
	}
}

// drawBar returns a drawn progress bar of a given width and percentage
// as a string. Examples:
//  [........................]
//  [###########.............]
//  [########################]
func drawBar(spaces int, percent float64) string {
	if spaces <= 0 {
		return ""
	}
	var strBuffer bytes.Buffer
	strBuffer.WriteString(BarLeft)

	// the number of "#" to draw
	fullSpaces := int(percent * float64(spaces))

	// some bounds for ensuring a constant width, even with weird inputs
	if fullSpaces > spaces {
		fullSpaces = spaces
	}
	if fullSpaces < 0 {
		fullSpaces = 0
	}

	// write the "#"s for the current percentage
	for i := 0; i < fullSpaces; i++ {
		strBuffer.WriteString(BarFilling)
	}
	// fill out the remainder of the bar
	for i := 0; i < spaces-fullSpaces; i++ {
		strBuffer.WriteString(BarEmpty)
	}
	strBuffer.WriteString(BarRight)
	return strBuffer.String()
}
