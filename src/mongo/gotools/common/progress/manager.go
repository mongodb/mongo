package progress

import (
	"fmt"
	"io"
	"sync"
	"time"

	"github.com/mongodb/mongo-tools/common/text"
)

// Manager is an interface which tools can use to registers progressors which
// track the progress of any arbitrary operation.
type Manager interface {
	// Attach registers the progressor with the manager under the given name.
	// Any call to Attach must have a matching call to Detach.
	Attach(name string, progressor Progressor)

	// Detach removes the progressor with the given name from the manager
	Detach(name string)
}

const GridPadding = 2

// BarWriter implements Manager. It periodically prints the status of all of its
// progressors in the form of pretty progress bars. It handles thread-safe
// synchronized progress bar writing, so that its progressors are written in a
// group at a given interval. It maintains insertion order when printing, such
// that new bars appear at the bottom of the group.
type BarWriter struct {
	sync.Mutex

	waitTime  time.Duration
	writer    io.Writer
	bars      []*Bar
	stopChan  chan struct{}
	barLength int
	isBytes   bool
}

// NewBarWriter returns an initialized BarWriter with the given bar length and
// byte-formatting toggle, waiting the given duration between writes
func NewBarWriter(w io.Writer, waitTime time.Duration, barLength int, isBytes bool) *BarWriter {
	return &BarWriter{
		waitTime:  waitTime,
		writer:    w,
		stopChan:  make(chan struct{}),
		barLength: barLength,
		isBytes:   isBytes,
	}
}

// Attach registers the given progressor with the manager
func (manager *BarWriter) Attach(name string, progressor Progressor) {
	pb := &Bar{
		Name:      name,
		Watching:  progressor,
		BarLength: manager.barLength,
		IsBytes:   manager.isBytes,
	}
	pb.validate()

	manager.Lock()
	defer manager.Unlock()

	// make sure we are not adding the same bar again
	for _, bar := range manager.bars {
		if bar.Name == name {
			panic(fmt.Sprintf("progress bar with name '%s' already exists in manager", name))
		}
	}

	manager.bars = append(manager.bars, pb)
}

// Detach removes the progressor with the given name from the manager. Insert
// order is maintained for consistent ordering of the printed bars.
func (manager *BarWriter) Detach(name string) {
	manager.Lock()
	defer manager.Unlock()
	var pb *Bar
	for _, bar := range manager.bars {
		if bar.Name == name {
			pb = bar
			break
		}
	}
	if pb == nil {
		panic("could not find progressor")
	}

	grid := &text.GridWriter{
		ColumnPadding: GridPadding,
	}
	if pb.hasRendered {
		// if we've rendered this bar at least once, render it one last time
		pb.renderToGridRow(grid)
	}
	grid.FlushRows(manager.writer)

	updatedBars := make([]*Bar, 0, len(manager.bars)-1)
	for _, bar := range manager.bars {
		// move all bars to the updated list except for the bar we want to detach
		if bar.Name != pb.Name {
			updatedBars = append(updatedBars, bar)
		}
	}

	manager.bars = updatedBars
}

// helper to render all bars in order
func (manager *BarWriter) renderAllBars() {
	manager.Lock()
	defer manager.Unlock()
	grid := &text.GridWriter{
		ColumnPadding: GridPadding,
	}
	for _, bar := range manager.bars {
		bar.renderToGridRow(grid)
	}
	grid.FlushRows(manager.writer)
	// add padding of one row if we have more than one active bar
	if len(manager.bars) > 1 {
		// we just write an empty array here, since a write call of any
		// length to our log.Writer will trigger a new logline.
		manager.writer.Write([]byte{})
	}
}

// Start kicks of the timed batch writing of progress bars.
func (manager *BarWriter) Start() {
	if manager.writer == nil {
		panic("Cannot use a progress.BarWriter with an unset Writer")
	}
	go manager.start()
}

func (manager *BarWriter) start() {
	if manager.waitTime <= 0 {
		manager.waitTime = DefaultWaitTime
	}
	ticker := time.NewTicker(manager.waitTime)
	defer ticker.Stop()

	for {
		select {
		case <-manager.stopChan:
			return
		case <-ticker.C:
			manager.renderAllBars()
		}
	}
}

// Stop ends the main manager goroutine, stopping the manager's bars
// from being rendered.
func (manager *BarWriter) Stop() {
	manager.stopChan <- struct{}{}
}
