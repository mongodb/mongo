package system

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"
)

type Shell struct {
	executor      Executor
	coverage      bool
	gobin         string
	reportsPath   string
	shortArgument string
}

func (self *Shell) GoTest(directory, packageName string) (output string, err error) {
	output, err = self.compilePackageDependencies(directory)
	if err == nil {
		output, err = self.goTest(directory, packageName)
	}
	return
}

func (self *Shell) compilePackageDependencies(directory string) (output string, err error) {
	return self.executor.Execute(directory, self.gobin, "test", "-i")
}

func (self *Shell) goTest(directory, packageName string) (output string, err error) {
	if !self.coverage {
		return self.runWithoutCoverage(directory, packageName)
	}

	return self.tryRunWithCoverage(directory, packageName)
}

func (self *Shell) tryRunWithCoverage(directory, packageName string) (output string, err error) {
	profileName := self.composeCoverageProfileName(packageName)
	output, err = self.runWithCoverage(directory, packageName, profileName+".txt")

	if err != nil && self.coverage {
		output, err = self.runWithoutCoverage(directory, packageName)
	} else if self.coverage {
		self.generateCoverageReports(directory, profileName+".txt", profileName+".html")
	}
	return
}

func (self *Shell) composeCoverageProfileName(packageName string) string {
	reportFilename := strings.Replace(packageName, "/", "-", -1)
	reportPath := filepath.Join(self.reportsPath, reportFilename)
	return reportPath
}

func (self *Shell) runWithCoverage(directory, packageName, profile string) (string, error) {
	arguments := []string{"test", "-v", self.shortArgument, "-covermode=set", "-coverprofile=" + profile}
	arguments = append(arguments, self.jsonFlag(directory, packageName)...)
	return self.executor.Execute(directory, self.gobin, arguments...)
}

func (self *Shell) runWithoutCoverage(directory, packageName string) (string, error) {
	arguments := []string{"test", "-v", self.shortArgument}
	arguments = append(arguments, self.jsonFlag(directory, packageName)...)
	return self.executor.Execute(directory, self.gobin, arguments...)
}

func (self *Shell) jsonFlag(directory, packageName string) []string {
	imports, err := self.executor.Execute(directory, self.gobin, "list", "-f", "'{{.TestImports}}'", packageName)
	if !strings.Contains(imports, goconveyDSLImport) && err == nil {
		return []string{}
	}
	return []string{"-json"}
}

func (self *Shell) generateCoverageReports(directory, profile, html string) {
	self.executor.Execute(directory, self.gobin, "tool", "cover", "-html="+profile, "-o", html)
}

func (self *Shell) Getenv(key string) string {
	return os.Getenv(key)
}

func (self *Shell) Setenv(key, value string) error {
	if self.Getenv(key) != value {
		return os.Setenv(key, value)
	}
	return nil
}

func NewShell(executor Executor, gobin string, short bool, cover bool, reports string) *Shell {
	self := new(Shell)
	self.executor = executor
	self.gobin = gobin
	self.shortArgument = fmt.Sprintf("-short=%t", short)
	self.coverage = cover
	self.reportsPath = reports
	return self
}

const (
	goconveyDSLImport = "github.com/smartystreets/goconvey/convey " // note the trailing space: we don't want to target packages nested in the /convey package.
)
