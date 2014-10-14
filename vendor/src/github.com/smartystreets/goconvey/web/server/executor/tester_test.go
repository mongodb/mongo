package executor

import (
	"errors"
	"fmt"
	"testing"
	"time"

	. "github.com/smartystreets/goconvey/convey"
	"github.com/smartystreets/goconvey/web/server/contract"
)

func TestConcurrentTester(t *testing.T) {
	Convey("Subject: Controlled execution of test packages", t, func() {
		fixture := NewTesterFixture()

		Convey("Whenever tests for each package are executed", func() {
			fixture.InBatchesOf(1).RunTests()

			Convey("The tester should execute the tests in each active package with the correct arguments",
				fixture.ShouldHaveRecordOfExecutionCommands)

			Convey("There should be a test output result for each active package",
				fixture.ShouldHaveOneOutputPerInput)

			Convey("The output should be as expected",
				fixture.OutputShouldBeAsExpected)
		})

		Convey("When the tests for each package are executed synchronously", func() {
			fixture.InBatchesOf(1).RunTests()

			Convey("Each active package should be run synchronously and in the given order",
				fixture.TestsShouldHaveRunContiguously)
		})

		Convey("When the tests for each package are executed synchronously with failures", func() {
			fixture.InBatchesOf(1).SetupFailedTestSuites().RunTests()

			Convey("The failed test packages should not result in any panics", func() {
				So(fixture.recovered, ShouldBeNil)
			})
		})

		Convey("When packages are tested concurrently", func() {
			fixture.InBatchesOf(concurrentBatchSize).RunTests()

			Convey("Active packages should be arranged and tested in batches of the appropriate size",
				fixture.TestsShouldHaveRunInBatchesOfTwo)
		})

		Convey("When packages are tested concurrently with failures", func() {
			fixture.InBatchesOf(concurrentBatchSize).SetupFailedTestSuites().RunTests()

			Convey("The failed test packages should not result in any panics", func() {
				So(fixture.recovered, ShouldBeNil)
			})
		})

		Convey("When running a test package produces no output and exits with an error", func() {
			fixture.InBatchesOf(1).SetupAbnormalError("This really shouldn't happen...").RunTests()

			Convey("Panic should ensue", func() {
				So(fixture.recovered.Error(), ShouldEqual, "This really shouldn't happen...")
			})
		})

		Convey("When running test packages concurrently and a test package produces no output and exits with an error", func() {
			fixture.InBatchesOf(concurrentBatchSize).SetupAbnormalError("This really shouldn't happen...").RunTests()

			Convey("Panic should ensue", func() {
				So(fixture.recovered.Error(), ShouldEqual, "This really shouldn't happen...")
			})
		})
	})
}

const concurrentBatchSize = 2

type TesterFixture struct {
	tester       *ConcurrentTester
	shell        *TimedShell
	results      []string
	compilations []*ShellCommand
	executions   []*ShellCommand
	packages     []*contract.Package
	recovered    error
}

func NewTesterFixture() *TesterFixture {
	self := new(TesterFixture)
	self.shell = NewTimedShell()
	self.tester = NewConcurrentTester(self.shell)
	self.packages = []*contract.Package{
		&contract.Package{Active: true, Path: "a"},
		&contract.Package{Active: true, Path: "b"},
		&contract.Package{Active: true, Path: "c"},
		&contract.Package{Active: true, Path: "d"},
		&contract.Package{Active: false, Path: "e"},
		&contract.Package{Active: true, Path: "f"},
	}
	return self
}

func (self *TesterFixture) InBatchesOf(batchSize int) *TesterFixture {
	self.tester.SetBatchSize(batchSize)
	return self
}

func (self *TesterFixture) SetupAbnormalError(message string) *TesterFixture {
	self.shell.setTripWire(message)
	return self
}

func (self *TesterFixture) SetupFailedTestSuites() *TesterFixture {
	self.shell.setExitWithError()
	return self
}

func (self *TesterFixture) RunTests() {
	defer func() {
		if r := recover(); r != nil {
			self.recovered = r.(error)
		}
	}()

	self.tester.TestAll(self.packages)
	for _, p := range self.packages {
		self.results = append(self.results, p.Output)
	}
	self.executions = self.shell.Executions()
}

func (self *TesterFixture) ShouldHaveRecordOfExecutionCommands() {
	expected := []string{"a", "b", "c", "d", "f"}
	actual := []string{}
	for _, pkg := range self.executions {
		actual = append(actual, pkg.Command)
	}
	So(actual, ShouldResemble, expected)
}

func (self *TesterFixture) ShouldHaveOneOutputPerInput() {
	So(len(self.results), ShouldEqual, len(self.packages))
}

func (self *TesterFixture) OutputShouldBeAsExpected() {
	for _, p := range self.packages {
		if p.Active {
			So(p.Output, ShouldEndWith, p.Path)
		} else {
			So(p.Output, ShouldBeBlank)
		}
		So(p.Error, ShouldBeNil)
	}
}

func (self *TesterFixture) TestsShouldHaveRunContiguously() {
	self.OutputShouldBeAsExpected()

	So(self.shell.MaxConcurrentCommands(), ShouldEqual, 1)

	for i := 0; i < len(self.executions)-1; i++ {
		current := self.executions[i]
		next := self.executions[i+1]
		So(current.Started, ShouldHappenBefore, next.Started)
		So(current.Ended, ShouldHappenOnOrBefore, next.Started)
	}
}

func (self *TesterFixture) TestsShouldHaveRunInBatchesOfTwo() {
	self.OutputShouldBeAsExpected()

	So(self.shell.MaxConcurrentCommands(), ShouldEqual, concurrentBatchSize)
}

/**** Fakes ****/

type ShellCommand struct {
	Command string
	Started time.Time
	Ended   time.Time
}

type TimedShell struct {
	executions   []*ShellCommand
	panicMessage string
	err          error
}

func (self *TimedShell) Executions() []*ShellCommand {
	return self.executions
}

func (self *TimedShell) MaxConcurrentCommands() int {
	var concurrent int

	for x, current := range self.executions {
		concurrentWith_x := 1
		for y, comparison := range self.executions {
			if y == x {
				continue
			} else if concurrentWith(current, comparison) {
				concurrentWith_x++
			}
		}
		if concurrentWith_x > concurrent {
			concurrent = concurrentWith_x
		}
	}
	return concurrent
}

func concurrentWith(current, comparison *ShellCommand) bool {
	return comparison.Started.After(current.Started) && comparison.Started.Before(current.Ended)
}

func (self *TimedShell) setTripWire(message string) {
	self.panicMessage = message
}

func (self *TimedShell) setExitWithError() {
	self.err = errors.New("Simulate test failure")
}

func (self *TimedShell) GoTest(directory, packageName string) (output string, err error) {
	if self.panicMessage != "" {
		return "", errors.New(self.panicMessage)
	}

	output = directory
	err = self.err
	self.executions = append(self.executions, self.composeCommand(directory))
	return
}

func (self *TimedShell) composeCommand(commandText string) *ShellCommand {
	start := time.Now()
	time.Sleep(nap)
	end := time.Now()
	return &ShellCommand{commandText, start, end}
}

func (self *TimedShell) Getenv(key string) string       { panic("NOT SUPPORTED") }
func (self *TimedShell) Setenv(key, value string) error { panic("NOT SUPPORTED") }

func NewTimedShell() *TimedShell {
	self := new(TimedShell)
	self.executions = []*ShellCommand{}
	return self
}

var nap, _ = time.ParseDuration("10ms")
var _ = fmt.Sprintf("fmt")
