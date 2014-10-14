package watcher

import (
	"log"
	"os"

	"github.com/smartystreets/goconvey/web/server/contract"
)

type Scanner struct {
	fileSystem         contract.FileSystem
	watcher            contract.Watcher
	root               string
	previous           int64
	latestFolders      map[string]bool
	preExistingFolders map[string]bool
}

func (self *Scanner) Scan() bool {
	rootIsNew := self.recordCurrentRoot()
	checksum, folders := self.analyzeCurrentFileSystemState()
	if !rootIsNew {
		self.notifyWatcherOfChangesInFolderStructure(folders)
	}
	self.preExistingFolders = folders
	return self.latestTestResultsAreStale(checksum)
}

func (self *Scanner) recordCurrentRoot() (changed bool) {
	root := self.watcher.Root()
	if root != self.root {
		log.Println("Updating root in scanner:", root)
		self.root = root
		return true
	}
	return false
}

func (self *Scanner) analyzeCurrentFileSystemState() (checksum int64, folders map[string]bool) {
	folders = make(map[string]bool)

	self.fileSystem.Walk(self.root, func(path string, info os.FileInfo, err error) error {
		step := newWalkStep(self.root, path, info, self.watcher)
		step.IncludeIn(folders)
		checksum += step.Sum()
		return nil
	})
	return checksum, folders
}

func (self *Scanner) notifyWatcherOfChangesInFolderStructure(latest map[string]bool) {
	self.accountForDeletedFolders(latest)
	self.accountForNewFolders(latest)
}
func (self *Scanner) accountForDeletedFolders(latest map[string]bool) {
	for folder, _ := range self.preExistingFolders {
		if _, exists := latest[folder]; !exists {
			self.watcher.Deletion(folder)
		}
	}
}
func (self *Scanner) accountForNewFolders(latest map[string]bool) {
	for folder, _ := range latest {
		if _, exists := self.preExistingFolders[folder]; !exists {
			self.watcher.Creation(folder)
		}
	}
}

func (self *Scanner) latestTestResultsAreStale(checksum int64) bool {
	defer func() { self.previous = checksum }()
	return self.previous != checksum
}

func NewScanner(fileSystem contract.FileSystem, watcher contract.Watcher) *Scanner {
	self := new(Scanner)
	self.fileSystem = fileSystem
	self.watcher = watcher
	self.latestFolders = make(map[string]bool)
	self.preExistingFolders = make(map[string]bool)
	self.rememberCurrentlyWatchedFolders()

	return self
}
func (self *Scanner) rememberCurrentlyWatchedFolders() {
	for _, item := range self.watcher.WatchedFolders() {
		self.preExistingFolders[item.Path] = true
	}
}
