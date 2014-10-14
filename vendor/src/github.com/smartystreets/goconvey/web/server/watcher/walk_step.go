package watcher

import (
	"os"
	"path/filepath"
	"strings"

	"github.com/smartystreets/goconvey/web/server/contract"
)

type walkStep struct {
	root    string
	path    string
	folder  string
	info    os.FileInfo
	watcher contract.Watcher
}

func (self *walkStep) IncludeIn(walked map[string]bool) {
	walked[self.folder] = true
}

func (self *walkStep) Sum() int64 {
	if self.watcher.IsIgnored(self.folder) || self.isIrrelevant() {
		return 0
	}

	if self.info.IsDir() {
		return 1
	}
	return self.info.Size() + self.info.ModTime().Unix()
}

func (self *walkStep) isIrrelevant() bool {
	return !self.isWatchedFolder() && !self.isWatchedFile()
}

func (self *walkStep) isWatchedFolder() bool {
	return strings.HasPrefix(self.path, self.root) &&
		self.info.IsDir()
}

func (self *walkStep) isWatchedFile() bool {
	return self.watcher.IsWatched(self.folder) &&
		filepath.Ext(self.path) == ".go" &&
		!strings.HasPrefix(filepath.Base(self.path), ".")
}

func newWalkStep(root, path string, info os.FileInfo, watcher contract.Watcher) *walkStep {
	self := new(walkStep)
	self.root = root
	self.path = path
	self.info = info
	self.folder = deriveFolderName(path, info)
	self.watcher = watcher
	return self
}

func deriveFolderName(path string, info os.FileInfo) string {
	if info.IsDir() {
		return path
	} else {
		return filepath.Dir(path)
	}
}
