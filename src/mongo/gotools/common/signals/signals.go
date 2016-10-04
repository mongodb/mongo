package signals

import (
	"github.com/mongodb/mongo-tools/common/log"
	"github.com/mongodb/mongo-tools/common/util"

	"os"
	"os/signal"
	"syscall"
)

// Handle is like HandleWithInterrupt but it doesn't take a finalizer and will
// exit immediately after the first signal is received.
func Handle() chan struct{} {
	return HandleWithInterrupt(nil)
}

// HandleWithInterrupt starts a goroutine which listens for SIGTERM, SIGINT, and
// SIGKILL and explicitly ignores SIGPIPE. It calls the finalizer function when
// the first signal is received and forcibly terminates the program after the
// second. If a nil function is provided, the program will exit after the first
// signal.
func HandleWithInterrupt(finalizer func()) chan struct{} {
	finishedChan := make(chan struct{})
	go handleSignals(finalizer, finishedChan)
	return finishedChan
}

func handleSignals(finalizer func(), finishedChan chan struct{}) {
	// explicitly ignore SIGPIPE; the tools should deal with write errors
	noopChan := make(chan os.Signal)
	signal.Notify(noopChan, syscall.SIGPIPE)

	log.Logv(log.DebugLow, "will listen for SIGTERM, SIGINT, and SIGKILL")
	sigChan := make(chan os.Signal, 2)
	signal.Notify(sigChan, syscall.SIGTERM, syscall.SIGINT, syscall.SIGKILL)
	defer signal.Stop(sigChan)
	if finalizer != nil {
		select {
		case sig := <-sigChan:
			// first signal use finalizer to terminate cleanly
			log.Logvf(log.Always, "signal '%s' received; attempting to shut down", sig)
			finalizer()
		case <-finishedChan:
			return
		}
	}
	select {
	case sig := <-sigChan:
		// second signal exits immediately
		log.Logvf(log.Always, "signal '%s' received; forcefully terminating", sig)
		os.Exit(util.ExitKill)
	case <-finishedChan:
		return
	}
}
