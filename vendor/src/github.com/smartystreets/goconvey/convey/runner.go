package convey

import (
	"fmt"

	"github.com/smartystreets/goconvey/convey/gotest"
	"github.com/smartystreets/goconvey/convey/reporting"
)

type runner struct {
	top         *scope
	active      *scope
	reporter    reporting.Reporter
	failureMode FailureMode

	awaitingNewStory bool
	focus            bool
}

func (self *runner) Begin(entry *registration) {
	self.active = self.top
	self.focus = entry.Focus

	self.ensureStoryCanBegin()
	self.reporter.BeginStory(reporting.NewStoryReport(entry.Test))
	self.Register(entry)
}
func (self *runner) ensureStoryCanBegin() {
	if self.awaitingNewStory {
		self.awaitingNewStory = false
	} else {
		panic(fmt.Sprintf("%s (See %s)", extraGoTest, gotest.FormatExternalFileAndLine()))
	}
}

func (self *runner) Register(entry *registration) {
	if self.focus && !entry.Focus {
		return
	}

	self.ensureStoryAlreadyStarted()

	child := newScope(entry, self.reporter)
	self.active.adopt(child)
}

func (self *runner) ensureStoryAlreadyStarted() {
	if self.awaitingNewStory {
		panic(missingGoTest)
	}
}

func (self *runner) RegisterReset(action *action) {
	self.active.registerReset(action)
}

func (self *runner) Run() {
	self.active = self.top
	self.failureMode = FailureHalts

	for !self.top.visited() {
		self.top.visit(self)
	}

	self.reporter.EndStory()
	self.awaitingNewStory = true
}

func newRunner() *runner {
	self := new(runner)

	self.reporter = newNilReporter()
	self.top = newScope(newRegistration(topLevel, newAction(func() {}, FailureInherits), nil), self.reporter)
	self.active = self.top
	self.awaitingNewStory = true

	return self
}

func (self *runner) UpgradeReporter(reporter reporting.Reporter) {
	self.reporter = reporter
}

func (self *runner) Report(result *reporting.AssertionResult) {
	self.reporter.Report(result)
	if result.Failure != "" && self.failureMode == FailureHalts {
		panic(failureHalt)
	}
}

func (self *runner) Write(content []byte) (written int, err error) {
	return self.reporter.Write(content)
}

func last(group []string) string {
	return group[len(group)-1]
}

const topLevel = "TOP"
const missingGoTest = `Top-level calls to Convey(...) need a reference to the *testing.T. 
    Hint: Convey("description here", t, func() { /* notice that the second argument was the *testing.T (t)! */ }) `
const extraGoTest = `Only the top-level call to Convey(...) needs a reference to the *testing.T.`
const failureHalt = "___FAILURE_HALT___"

//////////////////////// nilReporter /////////////////////////////

type nilReporter struct{}

func (self *nilReporter) BeginStory(story *reporting.StoryReport)  {}
func (self *nilReporter) Enter(scope *reporting.ScopeReport)       {}
func (self *nilReporter) Report(report *reporting.AssertionResult) {}
func (self *nilReporter) Exit()                                    {}
func (self *nilReporter) EndStory()                                {}
func (self *nilReporter) Write(p []byte) (int, error)              { return len(p), nil }
func newNilReporter() *nilReporter                                 { return new(nilReporter) }
