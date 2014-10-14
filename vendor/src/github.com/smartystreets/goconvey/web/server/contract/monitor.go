package contract

import "log"

type Monitor struct {
	scanner     Scanner
	watcher     Watcher
	executor    Executor
	server      Server
	paused      bool
	pauseUpdate chan bool
	sleep       func()
}

func (self *Monitor) ScanForever() {
	log.Println("Engaging monitoring loop...")
	for {
		self.Scan()
	}
}

func (self *Monitor) Scan() {
	self.updatePausedStatus()

	if !self.paused && self.scanner.Scan() {
		self.executeTests()
	} else {
		self.sleep()
	}
}

func (self *Monitor) updatePausedStatus() {
	select {
	case <-self.pauseUpdate:
		log.Println("Server is now paused:", !self.paused)
		self.paused = !self.paused
	default:
	}
}

func (self *Monitor) executeTests() {
	watched := self.watcher.WatchedFolders()

	log.Printf("Preparing for test run (watching %d folders)...\n", len(watched))
	output := self.executor.ExecuteTests(watched)

	log.Println("Test run complete, updating server with latest output...")
	self.server.ReceiveUpdate(output)

	log.Printf("Server updated with %d tested packages (revision: '%v').\n", len(output.Packages), output.Revision)
}

func NewMonitor(
	scanner Scanner,
	watcher Watcher,
	executor Executor,
	server Server,
	pauseUpdate chan bool,
	sleep func()) *Monitor {

	self := new(Monitor)
	self.scanner = scanner
	self.watcher = watcher
	self.executor = executor
	self.server = server
	self.pauseUpdate = pauseUpdate
	self.sleep = sleep
	return self
}
