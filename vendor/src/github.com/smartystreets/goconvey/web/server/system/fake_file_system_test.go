package system

import (
	"os"
	"testing"
	"time"

	. "github.com/smartystreets/goconvey/convey"
)

func TestFakeFileSystem(t *testing.T) {
	Convey("Subject: FakeFileSystem", t, func() {
		fs := NewFakeFileSystem()

		Convey("When walking a barren file system", func() {
			step := func(path string, info os.FileInfo, err error) error { panic("Should NOT happen!") }

			Convey("The step function should never be called", func() {
				So(func() { fs.Walk("/", step) }, ShouldNotPanic)
			})
		})

		Convey("When a file system is populated...", func() {
			first, second, third, fourth := time.Now(), time.Now(), time.Now(), time.Now()
			fs.Create("/root", 0, first)
			fs.Create("/root/a", 1, second)
			fs.Create("/elsewhere/c", 2, third)
			fs.Create("/root/b", 3, fourth)

			Convey("...and then walked", func() {
				paths, names, sizes, times, errors := []string{}, []string{}, []int64{}, []time.Time{}, []error{}
				fs.Walk("/root", func(path string, info os.FileInfo, err error) error {
					paths = append(paths, path)
					names = append(names, info.Name())
					sizes = append(sizes, info.Size())
					times = append(times, info.ModTime())
					errors = append(errors, err)
					return nil
				})

				Convey("Each nested path should be visited once", func() {
					So(paths, ShouldResemble, []string{"/root", "/root/a", "/root/b"})
					So(names, ShouldResemble, []string{"root", "a", "b"})
					So(sizes, ShouldResemble, []int64{0, 1, 3})
					So(times, ShouldResemble, []time.Time{first, second, fourth})
					So(errors, ShouldResemble, []error{nil, nil, nil})
				})
			})
		})

		Convey("When an existing file system item is modified", func() {
			fs.Create("/a.txt", 1, time.Now())
			fs.Modify("/a.txt")
			var size int64

			Convey("And the file system is then walked", func() {
				fs.Walk("/", func(path string, info os.FileInfo, err error) error {
					size = info.Size()
					return nil
				})
				Convey("The modification should be persistent", func() {
					So(size, ShouldEqual, 2)
				})
			})
		})

		Convey("When an existing file system item is renamed", func() {
			initial := time.Now()
			fs.Create("/a.txt", 1, initial)
			fs.Rename("/a.txt", "/z.txt")
			var modified time.Time
			var newName string

			Convey("And the file system is then walked", func() {
				fs.Walk("/", func(path string, info os.FileInfo, err error) error {
					modified = info.ModTime()
					newName = info.Name()
					return nil
				})
				Convey("The modification should be persistent", func() {
					So(modified, ShouldHappenAfter, initial)
					So(newName, ShouldEqual, "z.txt")
				})
			})
		})

		Convey("When an existing file system item is deleted", func() {
			fs.Create("/a.txt", 1, time.Now())
			fs.Delete("/a.txt")
			var found bool

			Convey("And the file system is then walked", func() {
				fs.Walk("/", func(path string, info os.FileInfo, err error) error {
					if info.Name() == "a.txt" {
						found = true
					}
					return nil
				})
				Convey("The deleted entry should NOT be visited", func() {
					So(found, ShouldBeFalse)
				})
			})
		})

		Convey("When an existing folder is deleted", func() {
			first, second, third := time.Now(), time.Now(), time.Now()
			fs.Create("/a", 1, first)
			fs.Create("/a/1.txt", 2, second)
			fs.Create("/b", 3, third)

			fs.Delete("/a")

			Convey("And the file system is then walked", func() {
				paths, names, sizes, times, errors := []string{}, []string{}, []int64{}, []time.Time{}, []error{}
				fs.Walk("/", func(path string, info os.FileInfo, err error) error {
					paths = append(paths, path)
					names = append(names, info.Name())
					sizes = append(sizes, info.Size())
					times = append(times, info.ModTime())
					errors = append(errors, err)
					return nil
				})

				Convey("All nested items and the folder should have been removed", func() {
					So(paths, ShouldResemble, []string{"/b"})
					So(names, ShouldResemble, []string{"b"})
					So(sizes, ShouldResemble, []int64{3})
					So(times, ShouldResemble, []time.Time{third})
					So(errors, ShouldResemble, []error{nil})
				})
			})
		})

		Convey("When a directory does NOT exist it should NOT be found", func() {
			So(fs.Exists("/not/there"), ShouldBeFalse)
		})

		Convey("When a folder is created", func() {
			modified := time.Now()
			fs.Create("/path/to/folder", 3, modified)

			Convey("It should be visible as a folder", func() {
				So(fs.Exists("/path/to/folder"), ShouldBeTrue)
			})
		})

		Convey("When a file is created", func() {
			fs.Create("/path/to/file.txt", 3, time.Now())

			Convey("It should NOT be visible as a folder", func() {
				So(fs.Exists("/path/to/file.txt"), ShouldBeFalse)
			})
		})

		Convey("Given a directory with contents", func() {
			fs.Create("/directory", 1, time.Now())
			fs.Create("/directory/1", 2, time.Now())
			fs.Create("/directory/2.txt", 3, time.Now())

			Convey("When a listing of the directory is requested", func() {
				contents, err := fs.Listing("/directory")

				Convey("The listing should contain all contents", func() {
					So(contents[0].Name(), ShouldEqual, "1")
					So(contents[0].IsDir(), ShouldBeTrue)
					So(contents[1].Name(), ShouldEqual, "2.txt")
					So(contents[1].IsDir(), ShouldBeFalse)
				})

				Convey("The operation should complete without error", func() {
					So(err, ShouldBeNil)
				})
			})
		})
	})
}
