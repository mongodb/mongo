package progress

import (
	"bytes"
	"fmt"
	"io"
	"time"
)

const (
	DefaultWaitTime = 3 * time.Second

	BarFilling = "#"
	BarEmpty   = "."
	BarLeft    = "["
	BarRight   = "]"
)

// ProgressBar is a tool for concurrently monitoring the progress
// of a task with a simple linear ASCII visualization
type ProgressBar struct {
	// Name is an identifier printed along with the bar
	Name string
	// Max is the maximum value the counter addressed at CounterPtr
	// is expected to reach (ie, the total)
	Max int
	// BarLength is the number of characters used to print the bar
	BarLength int
	// CounterPtr is a pointer to an integer the increases as progress
	// is made. The value pointed to is read periodically to draw the bar.
	CounterPtr *int
	// Writer is where the ProgressBar is written out to
	Writer io.Writer
	// WaitTime is the time to wait between writing the bar
	WaitTime time.Duration

	stopChan chan struct{}
}

// Start starts the ProgressBar goroutine. Once Start is called, a bar will
// be written to the given Writer at regular intervals. The goroutine
// can only be stopped manually using the Stop() method. The ProgressBar
// must be set up before calling this. Panics if Start has already been called.
func (pb *ProgressBar) Start() {
	pb.validate()
	pb.stopChan = make(chan struct{})

	go pb.start()
}

// validate does a set of sanity checks against the progress bar, and panics
// if the bar is unfit for use
func (pb *ProgressBar) validate() {
	if pb.CounterPtr == nil {
		panic("Cannot use a ProgressBar with an unset CounterPtr")
	}
	if pb.Writer == nil {
		panic("Cannot use a ProgressBar with an unset Writer")
	}
	if pb.stopChan != nil {
		panic("Cannot start a ProgressBar more than once")
	}
}

// Stop kills the ProgressBar goroutine, stopping it from writing.
// Generally called as
//  myProgressBar.Start()
//  defer myProgressBar.Stop()
// to stop leakage
func (pb *ProgressBar) Stop() {
	close(pb.stopChan)
}

// computes all necessary values renders to the bar's Writer
func (pb *ProgressBar) renderToWriter() {
	currentCount := *pb.CounterPtr
	percent := float64(currentCount) / float64(pb.Max)
	fmt.Fprintf(pb.Writer, "%v %v\t%d/%d (%2.1f%%)",
		drawBar(pb.BarLength, percent),
		pb.Name,
		currentCount,
		pb.Max,
		percent*100,
	)
}

// the main concurrent loop
func (pb *ProgressBar) start() {
	if pb.WaitTime <= 0 {
		pb.WaitTime = DefaultWaitTime
	}
	ticker := time.NewTicker(pb.WaitTime)
	defer ticker.Stop()

	for {
		select {
		case <-pb.stopChan:
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
