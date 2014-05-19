package options

import (
	. "github.com/smartystreets/goconvey/convey"
	"testing"
)

// mock version of extra options
type MockExtraOptions struct {
	RegisterCalled  bool
	PostParseCalled bool
	ValidateCalled  bool
	UsageCalled     bool
}

// funcs to satisfy the interface.  simply bookkeep that they have been called
func (self *MockExtraOptions) Register() {
	self.RegisterCalled = true
}

func (self *MockExtraOptions) PostParse() error {
	self.PostParseCalled = true
	return nil
}

func (self *MockExtraOptions) Validate() error {
	self.ValidateCalled = true
	return nil
}

func (self *MockExtraOptions) Usage() {
	self.UsageCalled = true
}

func TestAddingExtraOptions(t *testing.T) {

	var mongoToolOptions *MongoToolOptions

	Convey("With an instance of MongoToolOptions", t, func() {

		mongoToolOptions = &MongoToolOptions{}

		Convey("when using extra options", func() {

			Convey("adding extra options should cause them to be"+
				" registered", func() {

				extra := &MockExtraOptions{}
				mongoToolOptions.AddOptions(extra)
				So(extra.RegisterCalled, ShouldBeTrue)

			})

			Convey("the parsing and validating functions for the extra options"+
				" should be called automatically", func() {

				extra := &MockExtraOptions{}
				mongoToolOptions.AddOptions(extra)
				mongoToolOptions.ParseAndValidate()
				So(extra.PostParseCalled, ShouldBeTrue)
				So(extra.ValidateCalled, ShouldBeTrue)

			})

		})

	})
}
