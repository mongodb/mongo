package system

import (
	"io/ioutil"
	"log"
	"os"
	"path/filepath"
	"strings"
)

type FileSystem struct{}

func (self *FileSystem) Walk(root string, step filepath.WalkFunc) {
	err := filepath.Walk(root, func(path string, info os.FileInfo, err error) error {
		if self.isMetaDirectory(info) {
			return filepath.SkipDir
		}

		return step(path, info, err)
	})

	if err != nil && err != filepath.SkipDir {
		log.Println("Error while walking file system:", err)
		panic(err)
	}
}

func (self *FileSystem) isMetaDirectory(info os.FileInfo) bool {
	name := info.Name()
	return info.IsDir() && (strings.HasPrefix(name, ".") || strings.HasPrefix(name, "_") || name == "testdata")
}

func (self *FileSystem) Listing(directory string) ([]os.FileInfo, error) {
	return ioutil.ReadDir(directory)
}

func (self *FileSystem) Exists(directory string) bool {
	info, err := os.Stat(directory)
	return err == nil && info.IsDir()
}

func NewFileSystem() *FileSystem {
	return new(FileSystem)
}
