package watcher

import (
	"testing"
	"time"

	. "github.com/smartystreets/goconvey/convey"
	"github.com/smartystreets/goconvey/web/server/contract"
	"github.com/smartystreets/goconvey/web/server/system"
)

func TestScanner(t *testing.T) {
	var fixture *scannerFixture
	var changed bool

	Convey("To begin with, the scanner is provided a contrived file system environment", t, func() {
		fixture = newScannerFixture()

		Convey("When we call Scan() for the first time", func() {
			changed = fixture.scan()

			Convey("The scanner should report a change in state", func() {
				So(changed, ShouldBeTrue)
			})
		})

		Convey("Then, on subsequent calls to Scan()", func() {
			changed = fixture.scan()

			Convey("When the file system has not changed in any way", func() {

				Convey("The scanner should NOT report any change in state", func() {
					So(fixture.scan(), ShouldBeFalse)
				})
			})

			Convey("When a new go file is created within a watched folder", func() {
				fixture.fs.Create("/root/new_stuff.go", 42, time.Now())

				Convey("The scanner should report a change in state", func() {
					So(fixture.scan(), ShouldBeTrue)
				})
			})

			Convey("When a file that starts with . is created within a watched folder", func() {
				fixture.fs.Create("/root/.new_stuff.go", 42, time.Now())

				Convey("The scanner should not report a change in state", func() {
					So(fixture.scan(), ShouldBeFalse)
				})
			})

			Convey("When an existing go file within a watched folder has been modified", func() {
				fixture.fs.Modify("/root/sub/file.go")

				Convey("The scanner should report a change in state", func() {
					So(fixture.scan(), ShouldBeTrue)
				})
			})

			Convey("When an existing go file within a watched folder has been renamed", func() {
				fixture.fs.Rename("/root/sub/file.go", "/root/sub/asdf.go")

				Convey("The scanner should report a change in state", func() {
					So(fixture.scan(), ShouldBeTrue)
				})
			})

			Convey("When an existing go file within a watched folder has been deleted", func() {
				fixture.fs.Delete("/root/sub/file.go")

				Convey("The scanner should report a change in state", func() {
					So(fixture.scan(), ShouldBeTrue)
				})
			})

			Convey("When a go file is created outside any watched folders", func() {
				fixture.fs.Create("/outside/new_stuff.go", 42, time.Now())

				Convey("The scanner should NOT report a change in state", func() {
					So(fixture.scan(), ShouldBeFalse)
				})
			})

			Convey("When a go file is modified outside any watched folders", func() {
				fixture.fs.Create("/outside/new_stuff.go", 42, time.Now())
				fixture.scan() // reset

				Convey("The scanner should NOT report a change in state", func() {
					So(fixture.scan(), ShouldBeFalse)
				})
			})

			Convey("When a go file is renamed outside any watched folders", func() {
				fixture.fs.Create("/outside/new_stuff.go", 42, time.Now())
				fixture.scan() // reset
				fixture.fs.Rename("/outside/new_stuff.go", "/outside/newer_stoff.go")

				Convey("The scanner should NOT report a change in state", func() {
					So(fixture.scan(), ShouldBeFalse)
				})
			})

			Convey("When a go file is deleted outside any watched folders", func() {
				fixture.fs.Create("/outside/new_stuff.go", 42, time.Now())
				fixture.scan() // reset
				fixture.fs.Delete("/outside/new_stuff.go")

				Convey("The scanner should NOT report a change in state", func() {
					So(fixture.scan(), ShouldBeFalse)
				})
			})

			Convey("When a miscellaneous file is created", func() {
				fixture.fs.Create("/root/new_stuff.MISC", 42, time.Now())

				Convey("The scanner should NOT report a change in state", func() {
					So(fixture.scan(), ShouldBeFalse)
				})
			})

			Convey("When a miscellaneous file is modified", func() {
				fixture.fs.Create("/root/new_stuff.MISC", 42, time.Now())
				fixture.scan() // reset

				Convey("The scanner should NOT report a change in state", func() {
					So(fixture.scan(), ShouldBeFalse)
				})
			})

			Convey("When a miscellaneous file is renamed", func() {
				fixture.fs.Create("/root/new_stuff.MISC", 42, time.Now())
				fixture.scan() // reset
				fixture.fs.Rename("/root/new_stuff.MISC", "/root/newer_stoff.MISC")

				Convey("The scanner should NOT report a change in state", func() {
					So(fixture.scan(), ShouldBeFalse)
				})
			})

			Convey("When a miscellaneous file is deleted", func() {
				fixture.fs.Create("/root/new_stuff.MISC", 42, time.Now())
				fixture.scan() // reset
				fixture.fs.Delete("/root/new_stuff.MISC")

				Convey("The scanner should NOT report a change in state", func() {
					So(fixture.scan(), ShouldBeFalse)
				})
			})

			Convey("When a new folder is created inside a watched folder", func() {
				fixture.fs.Create("/root/new", 41, time.Now())
				changed := fixture.scan()

				Convey("The scanner should report the change", func() {
					So(changed, ShouldBeTrue)
				})

				Convey("The scanner should notify the watcher of the creation", func() {
					So(fixture.wasCreated("/root/new"), ShouldBeTrue)
				})
			})

			Convey("When an empty watched folder is deleted", func() {
				fixture.fs.Delete("/root/sub/empty")
				changed := fixture.scan()

				Convey("The scanner should report the change", func() {
					So(changed, ShouldBeTrue)
				})

				Convey("The scanner should notify the watcher of the deletion", func() {
					So(fixture.wasDeleted("/root/sub/empty"), ShouldBeTrue)
				})
			})

			Convey("When a folder is created outside any watched folders", func() {
				fixture.fs.Create("/outside/asdf", 41, time.Now())
				changed := fixture.scan()

				Convey("The scanner should NOT report the change", func() {
					So(changed, ShouldBeFalse)
				})

				Convey("The scanner should NOT notify the watcher of the change", func() {
					So(fixture.wasCreated("/outside/asdf"), ShouldBeFalse)
				})
			})

			Convey("When an ignored folder is deleted", func() {
				fixture.watcher.Ignore("/root/sub/empty")
				fixture.fs.Delete("/root/sub/empty")
				changed := fixture.scan()

				Convey("The scanner should report the change", func() {
					So(changed, ShouldBeTrue)
				})

				Convey("The scanner should notify the watcher of the change", func() {
					So(fixture.wasDeleted("/root/sub/empty"), ShouldBeTrue)
				})
			})

			// Once upon a time the scanner didn't keep track of the root internally.
			// This meant that when the scanner was instructed to scan a new root location
			// it appeared to the scanner that many of the internally stored folders had
			// been deleted becuase they were not part of the new root directory structure
			// and they were reported as deletions to the watcher, which was incorrect behavior.
			Convey("When the watcher has adjusted the root", func() {
				fixture.fs.Create("/somewhere", 3, time.Now())
				fixture.fs.Create("/somewhere/else", 3, time.Now())
				fixture.watcher.Adjust("/somewhere")

				Convey("And the scanner scans", func() {
					changed := fixture.scan()

					Convey("The scanner should report the change", func() {
						So(changed, ShouldBeTrue)
					})

					Convey("The scanner should NOT notify the watcher of incorrect folder deletions", func() {
						So(len(fixture.watcher.deleted), ShouldEqual, 0)
					})

					Convey("The scanner should NOT notify the watcher of incorrect folder creations", func() {
						So(len(fixture.watcher.created), ShouldEqual, 0)
					})
				})
			})

		})
	})
}

type scannerFixture struct {
	scanner *Scanner
	fs      *system.FakeFileSystem
	watcher *WatcherWrapper
}

func (self *scannerFixture) scan() bool {
	return self.scanner.Scan()
}
func (self *scannerFixture) wasDeleted(folder string) bool {
	return !self.wasCreated(folder)
}
func (self *scannerFixture) wasCreated(folder string) bool {
	for _, w := range self.watcher.WatchedFolders() {
		if w.Path == folder {
			return true
		}
	}
	return false
}

func newScannerFixture() *scannerFixture {
	self := new(scannerFixture)
	self.fs = system.NewFakeFileSystem()
	self.fs.Create("/root", 0, time.Now())
	self.fs.Create("/root/file.go", 1, time.Now())
	self.fs.Create("/root/sub", 0, time.Now())
	self.fs.Create("/root/sub/file.go", 2, time.Now())
	self.fs.Create("/root/sub/empty", 0, time.Now())
	self.watcher = newWatcherWrapper(NewWatcher(self.fs, system.NewFakeShell()))
	self.watcher.Adjust("/root")
	self.scanner = NewScanner(self.fs, self.watcher)
	return self
}

/******** WatcherWrapper ********/

type WatcherWrapper struct {
	inner   *Watcher
	created []string
	deleted []string
}

func (self *WatcherWrapper) WatchedFolders() []*contract.Package {
	return self.inner.WatchedFolders()
}

func (self *WatcherWrapper) Root() string {
	return self.inner.Root()
}

func (self *WatcherWrapper) Adjust(root string) error {
	return self.inner.Adjust(root)
}

func (self *WatcherWrapper) Deletion(folder string) {
	self.deleted = append(self.deleted, folder)
	self.inner.Deletion(folder)
}

func (self *WatcherWrapper) Creation(folder string) {
	self.created = append(self.created, folder)
	self.inner.Creation(folder)
}

func (self *WatcherWrapper) Ignore(folder string) {
	self.inner.Ignore(folder)
}

func (self *WatcherWrapper) Reinstate(folder string) {
	self.inner.Reinstate(folder)
}

func (self *WatcherWrapper) IsWatched(folder string) bool {
	return self.inner.IsWatched(folder)
}

func (self *WatcherWrapper) IsIgnored(folder string) bool {
	return self.inner.IsIgnored(folder)
}

func newWatcherWrapper(inner *Watcher) *WatcherWrapper {
	self := new(WatcherWrapper)
	self.inner = inner
	self.created = []string{}
	self.deleted = []string{}
	return self
}
