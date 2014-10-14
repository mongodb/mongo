package watcher

import (
	"errors"
	"fmt"
	"log"
	"os"
	"path/filepath"
	"strings"

	"github.com/smartystreets/goconvey/web/server/contract"
)

type Watcher struct {
	files          contract.FileSystem
	shell          contract.Shell
	watched        map[string]*contract.Package
	root           string
	ambientGoPaths []string
}

func (self *Watcher) Root() string {
	return self.root
}

func (self *Watcher) Adjust(root string) error {
	if !self.files.Exists(root) {
		return errors.New(fmt.Sprintf("Directory does not exist: '%s'", root))
	}

	log.Println("Adjusting to watch new root:", root)

	self.root = root
	self.watched = make(map[string]*contract.Package)
	self.files.Walk(root, self.includeFolders)

	return nil
}
func (self *Watcher) includeFolders(path string, info os.FileInfo, err error) error {
	if info.IsDir() {
		log.Println("Including:", path)
		self.watched[path] = contract.NewPackage(path)
	}
	return nil
}

func (self *Watcher) Deletion(folder string) {
	log.Println("Detected deletion of:", folder)
	delete(self.watched, folder)
}

func (self *Watcher) Creation(folder string) {
	log.Println("Detected creation of:", folder)
	self.watched[folder] = contract.NewPackage(folder)
}

func (self *Watcher) Ignore(packageNames string) {
	paths := strings.Split(packageNames, ";")
	for _, path := range paths {
		for key, value := range self.watched {
			if strings.HasSuffix(key, path) {
				value.Active = false
			}
		}
	}
}

func (self *Watcher) Reinstate(packageNames string) {
	paths := strings.Split(packageNames, ";")
	for _, path := range paths {
		for key, value := range self.watched {
			if strings.HasSuffix(key, path) {
				value.Active = true
			}
		}
	}
}
func (self *Watcher) WatchedFolders() []*contract.Package {
	i, watched := 0, make([]*contract.Package, len(self.watched))
	log.Println("Number of watched folders:", len(self.watched))
	for _, item := range self.watched {
		watched[i] = &contract.Package{
			Active: item.Active,
			Path:   item.Path,
			Name:   item.Name,
			Result: contract.NewPackageResult(item.Name),
		}
		i++
	}
	return watched
}

func (self *Watcher) IsWatched(folder string) bool {
	if value, exists := self.watched[folder]; exists {
		return value.Active
	}
	return false
}

func (self *Watcher) IsIgnored(folder string) bool {
	if value, exists := self.watched[folder]; exists {
		return !value.Active
	}
	return false
}

func NewWatcher(files contract.FileSystem, shell contract.Shell) *Watcher {
	self := new(Watcher)
	self.files = files
	self.shell = shell
	self.watched = map[string]*contract.Package{}
	goPath := self.shell.Getenv("GOPATH")
	self.ambientGoPaths = strings.Split(goPath, entrySeparator)
	return self
}

var entrySeparator = string(filepath.ListSeparator)
