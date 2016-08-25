package progress

import (
	"fmt"
	"github.com/mongodb/mongo-tools/common/text"
	"io"
	"sync"
	"time"
)

const GridPadding = 2

// Manager handles thread-safe synchronized progress bar writing, so that all
// given progress bars are written in a group at a given interval.
// The current implementation maintains insert order when printing,
// such that new bars appear at the bottom of the group.
type Manager struct {
	waitTime time.Duration
	writer   io.Writer
	bars     []*Bar
	barsLock *sync.Mutex
	stopChan chan struct{}
}

// NewProgressBarManager returns an initialized Manager with the given
// time.Duration to wait between writes
func NewProgressBarManager(w io.Writer, waitTime time.Duration) *Manager {
	return &Manager{
		waitTime: waitTime,
		writer:   w,
		barsLock: &sync.Mutex{},
		stopChan: make(chan struct{}),
	}
}

// Attach registers the given progress bar with the manager. Should be used as
//  myManager.Attach(myBar)
//  defer myManager.Detach(myBar)
func (manager *Manager) Attach(pb *Bar) {
	// first some quick error checks
	if pb.Name == "" {
		panic("cannot attach a nameless bar to a progress bar manager")
	}
	pb.validate()

	manager.barsLock.Lock()
	defer manager.barsLock.Unlock()

	// make sure we are not adding the same bar again
	for _, bar := range manager.bars {
		if bar.Name == pb.Name {
			panic(fmt.Sprintf("progress bar with name '%v' already exists in manager", pb.Name))
		}
	}

	manager.bars = append(manager.bars, pb)
}

// Detach removes the given progress bar from the manager.
// Insert order is maintained for consistent ordering of the printed bars.
//  Note: the manager removes progress bars by "Name" not by memory location
func (manager *Manager) Detach(pb *Bar) {
	if pb.Name == "" {
		panic("cannot detach a nameless bar from a progress bar manager")
	}

	manager.barsLock.Lock()
	defer manager.barsLock.Unlock()

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
func (manager *Manager) renderAllBars() {
	manager.barsLock.Lock()
	defer manager.barsLock.Unlock()
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
func (manager *Manager) Start() {
	if manager.writer == nil {
		panic("Cannot use a progress.Manager with an unset Writer")
	}
	go manager.start()
}

func (manager *Manager) start() {
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
func (manager *Manager) Stop() {
	manager.stopChan <- struct{}{}
}
