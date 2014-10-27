package progress

import (
	"fmt"
	"sync"
	"time"
)

// Manager handles thread-safe synchronized progress bar writing, so that all
// given progress bars are written in a group at a given interval.
// The current implementation maintains insert order when printing,
// such that new bars appear at the bottom of the group.
type Manager struct {
	WaitTime time.Duration

	bars     []*ProgressBar
	barsLock *sync.Mutex
	stopChan chan struct{}
}

// NewProgressBarManager returns an initialized Manager with the given
// time.Duration to wait between writes
func NewProgressBarManager(waitTime time.Duration) *Manager {
	return &Manager{
		WaitTime: waitTime,
		barsLock: &sync.Mutex{},
	}
}

// Attach registers the given progress bar with the manager. Should be used as
//  myManager.Attach(myBar)
//  defer myManager.Detach(myBar)
func (manager *Manager) Attach(pb *ProgressBar) {
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
			panic(fmt.Sprintf("progress bar with name '%v' already exists in manager"))
		}
	}

	manager.bars = append(manager.bars, pb)
}

// Detach removes the given progress bar from the manager.
// Insert order is maintained for consistent ordering of the printed bars.
//  Note: the manager removes progress bars by "Name" not by memory location
func (manager *Manager) Detach(pb *ProgressBar) {
	if pb.Name == "" {
		panic("cannot detach a nameless bar from a progress bar manager")
	}

	manager.barsLock.Lock()
	defer manager.barsLock.Unlock()

	updatedBars := make([]*ProgressBar, 0, len(manager.bars)-1)
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
	for _, bar := range manager.bars {
		bar.renderToWriter()
	}
}

// Start kicks of the timed batch writing of progress bars.
func (manager *Manager) Start() {
	// we make the stop channel here so that we can stop and restart a manager
	manager.stopChan = make(chan struct{})
	go manager.start()
}

func (manager *Manager) start() {
	if manager.WaitTime <= 0 {
		manager.WaitTime = DefaultWaitTime
	}
	ticker := time.NewTicker(manager.WaitTime)
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
	close(manager.stopChan)
}
