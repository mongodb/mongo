package mongoreplay

import (
	"fmt"
	"github.com/mongodb/mongo-tools/common/options"
	"runtime"
)

// Print the tool version to stdout.  Returns whether or not the version flag
// is specified.
func (o *Options) PrintVersion() bool {
	if o.Version {
		fmt.Printf("%v version: %v\n", "mongoreplay", options.VersionStr)
		fmt.Printf("git version: %v\n", options.Gitspec)
		fmt.Printf("Go version: %v\n", runtime.Version())
		fmt.Printf("   os: %v\n", runtime.GOOS)
		fmt.Printf("   arch: %v\n", runtime.GOARCH)
		fmt.Printf("   compiler: %v\n", runtime.Compiler)
	}
	return o.Version
}
