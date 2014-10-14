package contract

import (
	"path/filepath"
	"strings"

	"github.com/smartystreets/goconvey/convey/reporting"
)

type Package struct {
	Active bool
	Path   string
	Name   string
	Error  error
	Output string
	Result *PackageResult
}

func NewPackage(path string) *Package {
	self := new(Package)
	self.Active = true
	self.Path = path
	self.Name = resolvePackageName(path)
	self.Result = NewPackageResult(self.Name)
	return self
}

type CompleteOutput struct {
	Packages []*PackageResult
	Revision string
	Paused   bool
}

var ( // PackageResult.Outcome values:
	Ignored         = "ignored"
	Passed          = "passed"
	Failed          = "failed"
	Panicked        = "panicked"
	BuildFailure    = "build failure"
	NoTestFiles     = "no test files"
	NoTestFunctions = "no test functions"
	NoGoFiles       = "no go code"
)

type PackageResult struct {
	PackageName string
	Elapsed     float64
	Coverage    float64
	Outcome     string
	BuildOutput string
	TestResults []TestResult
}

func NewPackageResult(packageName string) *PackageResult {
	self := new(PackageResult)
	self.PackageName = packageName
	self.TestResults = []TestResult{}
	self.Coverage = -1
	return self
}

type TestResult struct {
	TestName string
	Elapsed  float64
	Passed   bool
	Skipped  bool
	File     string
	Line     int
	Message  string
	Error    string
	Stories  []reporting.ScopeResult

	RawLines []string `json:",omitempty"`
}

func NewTestResult(testName string) *TestResult {
	self := new(TestResult)
	self.Stories = []reporting.ScopeResult{}
	self.RawLines = []string{}
	self.TestName = testName
	return self
}

func resolvePackageName(path string) string {
	index := strings.Index(path, endGoPath)
	if index < 0 {
		return path
	}
	packageBeginning := index + len(endGoPath)
	name := path[packageBeginning:]
	return name
}

const (
	separator = string(filepath.Separator)
	endGoPath = separator + "src" + separator
)
