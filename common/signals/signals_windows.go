// Package signals properly handles platform-specific OS signals.
package signals

import (
	"github.com/mongodb/mongo-tools/common/util"
	"os"
	"os/signal"
)

func Handle() {
	// make the chan buffered to avoid a race where the signal comes in after we start notifying but before we start listening
	sigChan := make(chan os.Signal, 1)
	signal.Notify(sigChan, os.Interrupt, os.Kill)
	<-sigChan
	os.Exit(util.ExitKill)
}
