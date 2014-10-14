package system

import (
	"os"
	"path/filepath"
	"strings"
)

type DepthLimit struct {
	depth int
	inner fileSystem
}

func (self *DepthLimit) Walk(root string, step filepath.WalkFunc) {
	self.inner.Walk(root, func(path string, info os.FileInfo, err error) error {
		if self.withinDepth(root, path) {
			return step(path, info, err)
		}
		return filepath.SkipDir
	})
}

func (self *DepthLimit) withinDepth(root, path string) bool {
	if self.depth < 0 {
		return true
	}
	nested := path[len(root):]
	return strings.Count(nested, slash) <= self.depth
}

func (self *DepthLimit) Listing(directory string) ([]os.FileInfo, error) {
	return self.inner.Listing(directory)
}

func (self *DepthLimit) Exists(directory string) bool {
	return self.inner.Exists(directory)
}

func NewDepthLimit(inner fileSystem, depth int) *DepthLimit {
	self := new(DepthLimit)
	self.inner = inner
	self.depth = depth
	return self
}

const slash = string(os.PathSeparator)

/////////////////////////////////////////////

type fileSystem interface {
	Walk(root string, step filepath.WalkFunc)
	Listing(directory string) ([]os.FileInfo, error)
	Exists(directory string) bool
}
