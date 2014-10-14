package watcher

import (
	"errors"
	"io/ioutil"
	"log"
	"testing"
	"time"

	. "github.com/smartystreets/goconvey/convey"
	"github.com/smartystreets/goconvey/web/server/contract"
	"github.com/smartystreets/goconvey/web/server/system"
)

func TestWatcher(t *testing.T) {
	var (
		fixture         *watcherFixture
		expectedWatches interface{}
		actualWatches   interface{}
		expectedError   interface{}
		actualError     interface{}
	)

	Convey("Subject: Watcher", t, func() {
		log.SetOutput(ioutil.Discard)
		log.SetFlags(log.LstdFlags | log.Lshortfile)
		fixture = newWatcherFixture()

		Convey("When initialized there should be ZERO watched folders", func() {
			So(len(fixture.watched()), ShouldEqual, 0)
			So(fixture.watcher.Root(), ShouldBeBlank)
		})

		Convey("When pointing to a root folder", func() {
			actualWatches, expectedWatches = fixture.pointToExistingRoot(goProject)

			Convey("That folder should be included as the first watched folder", func() {
				So(actualWatches, ShouldResemble, expectedWatches)
			})

			Convey("That folder should be the new root", func() {
				So(fixture.watcher.Root(), ShouldEqual, goProject)
			})
		})

		Convey("When pointing to a root folder that does not exist", func() {
			actualError, expectedError = fixture.pointToImaginaryRoot("/not/there")

			Convey("An appropriate error should be returned", func() {
				So(actualError, ShouldResemble, expectedError)
			})

			Convey("The root should not be updated", func() {
				So(fixture.watcher.Root(), ShouldBeBlank)
			})
		})

		Convey("When pointing to a root folder with nested folders", func() {
			actualWatches, expectedWatches = fixture.pointToExistingRootWithNestedFolders()

			Convey("All nested folders should be added recursively to the watched folders", func() {
				So(actualWatches, ShouldResemble, expectedWatches)
			})
		})

		Convey("When the watcher is notified of a newly created folder", func() {
			actualWatches, expectedWatches = fixture.receiveNotificationOfNewFolder()

			Convey("The folder should be included in the watched folders", func() {
				So(actualWatches, ShouldResemble, expectedWatches)
			})
		})

		Convey("When the watcher is notified of a recently deleted folder", func() {
			actualWatches, expectedWatches = fixture.receiveNotificationOfDeletedFolder()

			Convey("The folder should no longer be included in the watched folders", func() {
				So(actualWatches, ShouldResemble, expectedWatches)
			})
		})

		Convey("When a watched folder is ignored", func() {
			actualWatches, expectedWatches = fixture.ignoreWatchedFolder()

			Convey("The folder should be marked as inactive in the watched folders listing", func() {
				So(actualWatches, ShouldResemble, expectedWatches)
			})
		})

		Convey("When multiple watched folders are ignored", func() {
			actualWatches, expectedWatches = fixture.ignoreWatchedFolders()
			Convey("The folders should be marked as inactive in the watched folders listing", func() {
				So(actualWatches, ShouldResemble, expectedWatches)
			})
		})

		Convey("When a folder that is not being watched is ignored", func() {
			actualWatches, expectedWatches = fixture.ignoreIrrelevantFolder()

			Convey("The request should be ignored", func() {
				So(actualWatches, ShouldResemble, expectedWatches)
			})
		})

		Convey("When a folder that does not exist is ignored", func() {
			actualWatches, expectedWatches = fixture.ignoreImaginaryFolder()

			Convey("There should be no change to the watched folders", func() {
				So(actualWatches, ShouldResemble, expectedWatches)
			})
		})

		Convey("When an ignored folder is reinstated", func() {
			actualWatches, expectedWatches = fixture.reinstateIgnoredFolder()

			Convey("The folder should be included once more in the watched folders", func() {
				So(actualWatches, ShouldResemble, expectedWatches)
			})
		})

		Convey("When multiple ignored folders are reinstated", func() {
			actualWatches, expectedWatches = fixture.reinstateIgnoredFolders()

			Convey("The folders should be included once more in the watched folders", func() {
				So(actualWatches, ShouldResemble, expectedWatches)
			})
		})

		Convey("When an ignored folder is deleted and then reinstated", func() {
			actualWatches, expectedWatches = fixture.reinstateDeletedFolder()

			Convey("The reinstatement request should be ignored", func() {
				So(actualWatches, ShouldResemble, expectedWatches)
			})
		})

		Convey("When a folder that is not being watched is reinstated", func() {
			actualWatches, expectedWatches = fixture.reinstateIrrelevantFolder()

			Convey("The request should be ignored", func() {
				So(actualWatches, ShouldResemble, expectedWatches)
			})
		})

		Convey("Regardless of the status of the watched folders", func() {
			folders := fixture.setupSeveralFoldersWithWatcher()

			Convey("The IsWatched query method should be correct", func() {
				So(fixture.watcher.IsWatched(folders["active"]), ShouldBeTrue)
				So(fixture.watcher.IsWatched(folders["reinstated"]), ShouldBeTrue)

				So(fixture.watcher.IsWatched(folders["ignored"]), ShouldBeFalse)
				So(fixture.watcher.IsWatched(folders["deleted"]), ShouldBeFalse)
				So(fixture.watcher.IsWatched(folders["irrelevant"]), ShouldBeFalse)
			})

			Convey("The IsIgnored query method should be correct", func() {
				So(fixture.watcher.IsIgnored(folders["ignored"]), ShouldBeTrue)

				So(fixture.watcher.IsIgnored(folders["active"]), ShouldBeFalse)
				So(fixture.watcher.IsIgnored(folders["reinstated"]), ShouldBeFalse)
				So(fixture.watcher.IsIgnored(folders["deleted"]), ShouldBeFalse)
				So(fixture.watcher.IsIgnored(folders["irrelevant"]), ShouldBeFalse)
			})
		})
	})
}

type watcherFixture struct {
	watcher *Watcher
	files   *system.FakeFileSystem
	shell   *system.FakeShell
}

func (self *watcherFixture) watched() []*contract.Package {
	return self.watcher.WatchedFolders()
}

func (self *watcherFixture) pointToExistingRoot(folder string) (actual, expected interface{}) {
	self.files.Create(folder, 1, time.Now())

	self.watcher.Adjust(folder)

	actual = self.watched()
	expected = []*contract.Package{&contract.Package{Active: true, Path: goProject, Name: goPackagePrefix, Result: contract.NewPackageResult(goPackagePrefix)}}
	return
}

func (self *watcherFixture) pointToImaginaryRoot(folder string) (actual, expected interface{}) {
	actual = self.watcher.Adjust(folder)
	expected = errors.New("Directory does not exist: '/not/there'")
	return
}

func (self *watcherFixture) pointToExistingRootWithNestedFolders() (actual, expected interface{}) {
	self.files.Create(goProject, 1, time.Now())
	self.files.Create(goProject+"/sub", 2, time.Now())
	self.files.Create(goProject+"/sub2", 3, time.Now())
	self.files.Create(goProject+"/sub/subsub", 4, time.Now())

	self.watcher.Adjust(goProject)

	actual = self.watched()
	expected = []*contract.Package{
		&contract.Package{Active: true, Path: goProject, Name: goPackagePrefix, Result: contract.NewPackageResult(goPackagePrefix)},
		&contract.Package{Active: true, Path: goProject + "/sub", Name: goPackagePrefix + "/sub", Result: contract.NewPackageResult(goPackagePrefix + "/sub")},
		&contract.Package{Active: true, Path: goProject + "/sub2", Name: goPackagePrefix + "/sub2", Result: contract.NewPackageResult(goPackagePrefix + "/sub2")},
		&contract.Package{Active: true, Path: goProject + "/sub/subsub", Name: goPackagePrefix + "/sub/subsub", Result: contract.NewPackageResult(goPackagePrefix + "/sub/subsub")},
	}
	return
}

func (self *watcherFixture) pointToRootOfGoPath() {
	self.files.Create("/root/gopath", 5, time.Now())

	self.watcher.Adjust("/root/gopath")
}

func (self *watcherFixture) pointToNestedPartOfGoPath() {
	self.files.Create("/root/gopath", 5, time.Now())
	self.files.Create("/root/gopath/src/github.com/smartystreets/project", 6, time.Now())

	self.watcher.Adjust("/root/gopath/src/github.com/smartystreets/project")
}

func (self *watcherFixture) pointTo(path string) {
	self.files.Create(path, 5, time.Now())
	self.watcher.Adjust(path)
}

func (self *watcherFixture) setAmbientGoPath(path string) {
	self.shell.Setenv("GOPATH", path)
	self.files.Create(path, int64(42+len(path)), time.Now())
	self.watcher = NewWatcher(self.files, self.shell)
}

func (self *watcherFixture) receiveNotificationOfNewFolder() (actual, expected interface{}) {
	self.watcher.Creation(goProject + "/sub")

	actual = self.watched()
	expected = []*contract.Package{&contract.Package{Active: true, Path: goProject + "/sub", Name: goPackagePrefix + "/sub", Result: contract.NewPackageResult(goPackagePrefix + "/sub")}}
	return
}

func (self *watcherFixture) receiveNotificationOfDeletedFolder() (actual, expected interface{}) {
	self.watcher.Creation(goProject + "/sub2")
	self.watcher.Creation(goProject + "/sub")

	self.watcher.Deletion(goProject + "/sub")

	actual = self.watched()
	expected = []*contract.Package{&contract.Package{Active: true, Path: goProject + "/sub2", Name: goPackagePrefix + "/sub2", Result: contract.NewPackageResult(goPackagePrefix + "/sub2")}}
	return
}

func (self *watcherFixture) ignoreWatchedFolder() (actual, expected interface{}) {
	self.watcher.Creation(goProject + "/sub2")

	self.watcher.Ignore(goPackagePrefix + "/sub2")

	actual = self.watched()
	expected = []*contract.Package{&contract.Package{Active: false, Path: goProject + "/sub2", Name: goPackagePrefix + "/sub2", Result: contract.NewPackageResult(goPackagePrefix + "/sub2")}}
	return
}

func (self *watcherFixture) ignoreWatchedFolders() (actual, expected interface{}) {
	self.watcher.Creation(goProject + "/sub2")
	self.watcher.Creation(goProject + "/sub3")
	self.watcher.Creation(goProject + "/sub4")

	self.watcher.Ignore(goPackagePrefix + "/sub2;" + goPackagePrefix + "/sub4")

	actual = self.watched()
	expected = []*contract.Package{
		&contract.Package{Active: false, Path: goProject + "/sub2", Name: goPackagePrefix + "/sub2", Result: contract.NewPackageResult(goPackagePrefix + "/sub2")},
		&contract.Package{Active: true, Path: goProject + "/sub3", Name: goPackagePrefix + "/sub3", Result: contract.NewPackageResult(goPackagePrefix + "/sub3")},
		&contract.Package{Active: false, Path: goProject + "/sub4", Name: goPackagePrefix + "/sub4", Result: contract.NewPackageResult(goPackagePrefix + "/sub4")},
	}
	return
}

func (self *watcherFixture) ignoreIrrelevantFolder() (actual, expected interface{}) {
	self.files.Create(goProject, 1, time.Now())
	self.files.Create("/something", 1, time.Now())
	self.watcher.Adjust(goProject)

	self.watcher.Ignore("/something")

	actual = self.watched()
	expected = []*contract.Package{&contract.Package{Active: true, Path: goProject, Name: goPackagePrefix, Result: contract.NewPackageResult(goPackagePrefix)}}
	return
}

func (self *watcherFixture) ignoreImaginaryFolder() (actual, expected interface{}) {
	self.files.Create(goProject, 1, time.Now())
	self.watcher.Adjust(goProject)

	self.watcher.Ignore("/not/there")

	actual = self.watched()
	expected = []*contract.Package{&contract.Package{Active: true, Path: goProject, Name: goPackagePrefix, Result: contract.NewPackageResult(goPackagePrefix)}}
	return
}

func (self *watcherFixture) reinstateIgnoredFolder() (actual, expected interface{}) {
	self.files.Create(goProject, 1, time.Now())
	self.files.Create(goProject+"/sub", 2, time.Now())
	self.watcher.Adjust(goProject)
	self.watcher.Ignore(goPackagePrefix + "/sub")

	self.watcher.Reinstate(goProject + "/sub")

	actual = self.watched()
	expected = []*contract.Package{
		&contract.Package{Active: true, Path: goProject, Name: goPackagePrefix, Result: contract.NewPackageResult(goPackagePrefix)},
		&contract.Package{Active: true, Path: goProject + "/sub", Name: goPackagePrefix + "/sub", Result: contract.NewPackageResult(goPackagePrefix + "/sub")},
	}
	return
}

func (self *watcherFixture) reinstateIgnoredFolders() (actual, expected interface{}) {
	self.files.Create(goProject, 1, time.Now())
	self.files.Create(goProject+"/sub", 2, time.Now())
	self.files.Create(goProject+"/sub2", 3, time.Now())
	self.files.Create(goProject+"/sub3", 4, time.Now())
	self.watcher.Adjust(goProject)
	self.watcher.Ignore(goPackagePrefix + "/sub;" + goPackagePrefix + "/sub2;" + goPackagePrefix + "/sub3")

	self.watcher.Reinstate(goProject + "/sub;" + goPackagePrefix + "/sub3")

	actual = self.watched()
	expected = []*contract.Package{
		&contract.Package{Active: true, Path: goProject, Name: goPackagePrefix, Result: contract.NewPackageResult(goPackagePrefix)},
		&contract.Package{Active: true, Path: goProject + "/sub", Name: goPackagePrefix + "/sub", Result: contract.NewPackageResult(goPackagePrefix + "/sub")},
		&contract.Package{Active: false, Path: goProject + "/sub2", Name: goPackagePrefix + "/sub2", Result: contract.NewPackageResult(goPackagePrefix + "/sub2")},
		&contract.Package{Active: true, Path: goProject + "/sub3", Name: goPackagePrefix + "/sub3", Result: contract.NewPackageResult(goPackagePrefix + "/sub3")},
	}
	return
}

func (self *watcherFixture) reinstateDeletedFolder() (actual, expected interface{}) {
	self.files.Create(goProject, 1, time.Now())
	self.files.Create(goProject+"/sub", 2, time.Now())
	self.watcher.Adjust(goProject)
	self.watcher.Ignore(goPackagePrefix + "/sub")
	self.watcher.Deletion(goProject + "/sub")

	self.watcher.Reinstate(goPackagePrefix + "/sub")

	actual = self.watched()
	expected = []*contract.Package{&contract.Package{Active: true, Path: goProject, Name: goPackagePrefix, Result: contract.NewPackageResult(goPackagePrefix)}}
	return
}

func (self *watcherFixture) reinstateIrrelevantFolder() (actual, expected interface{}) {
	self.files.Create(goProject, 1, time.Now())
	self.files.Create("/irrelevant", 2, time.Now())
	self.watcher.Adjust(goProject)

	self.watcher.Reinstate("/irrelevant")

	actual = self.watched()
	expected = []*contract.Package{&contract.Package{Active: true, Path: goProject, Name: goPackagePrefix, Result: contract.NewPackageResult(goPackagePrefix)}}
	return
}

func (self *watcherFixture) setupSeveralFoldersWithWatcher() map[string]string {
	self.files.Create(goProject, 0, time.Now())
	self.files.Create(goProject+"/active", 1, time.Now())
	self.files.Create(goProject+"/reinstated", 2, time.Now())
	self.files.Create(goProject+"/ignored", 3, time.Now())
	self.files.Create(goProject+"/deleted", 4, time.Now())
	self.files.Create("/irrelevant", 5, time.Now())

	self.watcher.Adjust(goProject)
	self.watcher.Ignore(goPackagePrefix + "/ignored")
	self.watcher.Ignore(goPackagePrefix + "/reinstated")
	self.watcher.Reinstate(goPackagePrefix + "/reinstated")
	self.watcher.Deletion(goProject + "/deleted")
	self.files.Delete(goProject + "/deleted")

	return map[string]string{
		"active":     goProject + "/active",
		"reinstated": goProject + "/reinstated",
		"ignored":    goProject + "/ignored",
		"deleted":    goProject + "/deleted",
		"irrelevant": "/irrelevant",
	}
}

func newWatcherFixture() *watcherFixture {
	self := new(watcherFixture)
	self.files = system.NewFakeFileSystem()
	self.shell = system.NewFakeShell()
	self.shell.Setenv("GOPATH", gopath)
	self.watcher = NewWatcher(self.files, self.shell)
	return self
}

const gopath = "/root/gopath"
const goPackagePrefix = "github.com/smartystreets/project"
const goProject = gopath + "/src/" + goPackagePrefix
