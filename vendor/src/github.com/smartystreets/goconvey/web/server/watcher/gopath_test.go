package watcher

import (
	"testing"

	. "github.com/smartystreets/goconvey/convey"
	"github.com/smartystreets/goconvey/web/server/system"
)

func TestGoPath(t *testing.T) {
	var fixture *goPathFixture

	Convey("Subject: goPath abstracts the $GOPATH environment variable", t, func() {
		fixture = newGoPathFixture()

		Convey("Package names should be resolved from paths in consultation with the $GOPATH", func() {
			for packagePath, expected := range resolutions {
				So(fixture.gopath.ResolvePackageName(packagePath), ShouldEqual, expected)
			}
		})

		Convey("Panic should ensue if package name resolution is attempted outside any available workspace", func() {
			defer func() {
				recovered := recover()
				if recovered == nil {
					So(recovered, ShouldNotBeNil)
				} else {
					So(recovered, ShouldStartWith, resolutionError)
				}
			}()
			fixture.gopath.ResolvePackageName("/blah/src/package")
		})
	})
}

type goPathFixture struct {
	shell  *system.FakeShell
	gopath *goPath
}

func newGoPathFixture() *goPathFixture {
	self := new(goPathFixture)
	self.shell = system.NewFakeShell()
	self.shell.Setenv("GOPATH", all)
	self.gopath = newGoPath(self.shell)
	return self
}

const ( // workspaces
	basic    = "/root/gopath"
	newBasic = "/root/otherGopath"
	nested   = "/root/src/gopath"
	crazy    = "/src/github.com"

	all = basic + delimiter + newBasic + delimiter + nested + delimiter + crazy
)

var resolutions = map[string]string{
	"/root/gopath/src/package":                          "package",
	"/root/gopath/src/github.com/package":               "github.com/package",
	"/root/gopath/src/github.com/project/package1":      "github.com/project/package1",
	"/root/otherGopath/src/github.com/project/package2": "github.com/project/package2",
	"/root/src/gopath/src/github.com/project/package3":  "github.com/project/package3",

	// This crazy test case illustrates the point that "/src/" whether indexed at the beginning of the
	// string or the end of the string may not always be the correct way to resolve the package name.
	// In this case, the workspace contains a "src", there is a "src" that connects the workspace to
	// the package (as expected), and there is a "src" in the actual package name.
	// Dear reader, please, don't ever, ever structure your go code like this!
	"/src/github.com/src/github.com/project/src/package": "github.com/project/src/package",
}
