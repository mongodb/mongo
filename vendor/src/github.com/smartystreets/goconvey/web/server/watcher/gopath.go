package watcher

import (
	"fmt"
	"path/filepath"
	"strings"

	"github.com/smartystreets/goconvey/web/server/contract"
)

type goPath struct {
	shell contract.Shell
}

func (self *goPath) ResolvePackageName(folder string) string {
	for _, workspace := range strings.Split(self.current(), delimiter) {
		if strings.HasPrefix(folder, workspace) {
			prefix := filepath.Join(workspace, "src") + separator
			return folder[len(prefix):]
		}
	}

	panic(fmt.Sprintln(resolutionError, self.current()))
}

func (self *goPath) current() string {
	return self.shell.Getenv("GOPATH")
}

func newGoPath(shell contract.Shell) *goPath {
	self := new(goPath)
	self.shell = shell
	return self
}

const delimiter = string(filepath.ListSeparator)
const separator = string(filepath.Separator)
const resolutionError = "Package cannot be resolved as it is outside of any workspaces listed in the current $GOPATH:"
