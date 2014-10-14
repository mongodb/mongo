package contract

import (
	"net/http"
	"os"
	"path/filepath"
)

type (
	Server interface {
		ReceiveUpdate(*CompleteOutput)
		Watch(writer http.ResponseWriter, request *http.Request)
		Ignore(writer http.ResponseWriter, request *http.Request)
		Reinstate(writer http.ResponseWriter, request *http.Request)
		Status(writer http.ResponseWriter, request *http.Request)
		LongPollStatus(writer http.ResponseWriter, request *http.Request)
		Results(writer http.ResponseWriter, request *http.Request)
		Execute(writer http.ResponseWriter, request *http.Request)
		TogglePause(writer http.ResponseWriter, request *http.Request)
	}

	Executor interface {
		ExecuteTests([]*Package) *CompleteOutput
		Status() string
		ClearStatusFlag() bool
	}

	Scanner interface {
		Scan() (changed bool)
	}

	Watcher interface {
		Root() string
		Adjust(root string) error

		Deletion(folder string)
		Creation(folder string)

		Ignore(folders string)
		Reinstate(folders string)

		WatchedFolders() []*Package
		IsWatched(folder string) bool
		IsIgnored(folder string) bool
	}

	FileSystem interface {
		Walk(root string, step filepath.WalkFunc)
		Listing(directory string) ([]os.FileInfo, error)
		Exists(directory string) bool
	}

	Shell interface {
		GoTest(directory, packageName string) (output string, err error)
		Getenv(key string) string
		Setenv(key, value string) error
	}
)
