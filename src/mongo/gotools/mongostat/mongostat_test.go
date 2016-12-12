package mongostat

import (
	"io/ioutil"
	"strings"
	"testing"
	"time"

	"github.com/mongodb/mongo-tools/common/testutil"
	"github.com/mongodb/mongo-tools/mongostat/stat_consumer/line"
	"github.com/mongodb/mongo-tools/mongostat/status"
	. "github.com/smartystreets/goconvey/convey"
	"gopkg.in/mgo.v2/bson"
)

func readBSONFile(file string, t *testing.T) (stat *status.ServerStatus) {
	stat = &status.ServerStatus{}
	ssBSON, err := ioutil.ReadFile(file)
	if err == nil {
		err = bson.Unmarshal(ssBSON, stat)
	}
	if err != nil {
		t.Logf("Could not load new ServerStatus BSON: %s", err)
		t.FailNow()
	}
	return
}

func TestStatLine(t *testing.T) {
	testutil.VerifyTestType(t, testutil.UnitTestType)

	defaultHeaders := make([]string, len(line.CondHeaders))
	for i, h := range line.CondHeaders {
		defaultHeaders[i] = h.Key
	}
	defaultConfig := &status.ReaderConfig{
		HumanReadable: true,
	}

	serverStatusOld := readBSONFile("test_data/server_status_old.bson", t)
	serverStatusNew := readBSONFile("test_data/server_status_new.bson", t)
	serverStatusNew.ShardCursorType = nil
	serverStatusOld.ShardCursorType = nil

	Convey("StatsLine should accurately calculate opcounter diffs", t, func() {
		statsLine := line.NewStatLine(serverStatusOld, serverStatusNew, defaultHeaders, defaultConfig)
		So(statsLine.Fields["insert"], ShouldEqual, "10")
		So(statsLine.Fields["query"], ShouldEqual, "5")
		So(statsLine.Fields["update"], ShouldEqual, "7")
		So(statsLine.Fields["delete"], ShouldEqual, "2")
		So(statsLine.Fields["getmore"], ShouldEqual, "3")
		command := strings.Split(statsLine.Fields["command"], "|")[0]
		So(command, ShouldEqual, "669")
		So(statsLine.Fields["faults"], ShouldEqual, "5")

		locked := strings.Split(statsLine.Fields["locked_db"], ":")
		So(locked[0], ShouldEqual, "test")
		So(locked[1], ShouldEqual, "50.0%")
		qrw := strings.Split(statsLine.Fields["qrw"], "|")
		So(qrw[0], ShouldEqual, "3")
		So(qrw[1], ShouldEqual, "2")
		arw := strings.Split(statsLine.Fields["arw"], "|")
		So(arw[0], ShouldEqual, "4")
		So(arw[1], ShouldEqual, "6")
		So(statsLine.Fields["net_in"], ShouldEqual, "2.00k")
		So(statsLine.Fields["net_out"], ShouldEqual, "3.00k")
		So(statsLine.Fields["conn"], ShouldEqual, "5")
	})

	serverStatusNew.SampleTime, _ = time.Parse("2006 Jan 02 15:04:05", "2015 Nov 30 4:25:33")
	Convey("StatsLine with non-default interval should calculate average diffs", t, func() {
		statsLine := line.NewStatLine(serverStatusOld, serverStatusNew, defaultHeaders, defaultConfig)
		// Opcounters and faults are averaged over sample period
		So(statsLine.Fields["insert"], ShouldEqual, "3")
		So(statsLine.Fields["query"], ShouldEqual, "1")
		So(statsLine.Fields["update"], ShouldEqual, "2")
		delete := strings.TrimPrefix(statsLine.Fields["delete"], "*")
		So(delete, ShouldEqual, "0")
		So(statsLine.Fields["getmore"], ShouldEqual, "1")
		command := strings.Split(statsLine.Fields["command"], "|")[0]
		So(command, ShouldEqual, "223")
		So(statsLine.Fields["faults"], ShouldEqual, "1")

		locked := strings.Split(statsLine.Fields["locked_db"], ":")
		So(locked[0], ShouldEqual, "test")
		So(locked[1], ShouldEqual, "50.0%")
		qrw := strings.Split(statsLine.Fields["qrw"], "|")
		So(qrw[0], ShouldEqual, "3")
		So(qrw[1], ShouldEqual, "2")
		arw := strings.Split(statsLine.Fields["arw"], "|")
		So(arw[0], ShouldEqual, "4")
		So(arw[1], ShouldEqual, "6")
		So(statsLine.Fields["net_in"], ShouldEqual, "666b")
		So(statsLine.Fields["net_out"], ShouldEqual, "1.00k")
		So(statsLine.Fields["conn"], ShouldEqual, "5")
	})
}

func TestIsMongos(t *testing.T) {
	testutil.VerifyTestType(t, testutil.UnitTestType)

	runCheck := func(process string) bool {
		return status.IsMongos(&status.ServerStatus{
			Process: process,
		})
	}

	Convey("should accept reasonable process names", t, func() {
		So(runCheck("/mongos-prod.exe"), ShouldBeTrue)
		So(runCheck("/mongos.exe"), ShouldBeTrue)
		So(runCheck("mongos"), ShouldBeTrue)
		So(runCheck("mongodb/bin/mongos"), ShouldBeTrue)
		So(runCheck(`C:\data\mci\48de1dc1ec3c2be5dcd6a53739578de4\src\mongos.exe`), ShouldBeTrue)
	})
	Convey("should accept reasonable process names", t, func() {
		So(runCheck("mongosx/mongod"), ShouldBeFalse)
		So(runCheck("mongostat"), ShouldBeFalse)
		So(runCheck("mongos_stuff/mongod"), ShouldBeFalse)
		So(runCheck("mongos.stuff/mongod"), ShouldBeFalse)
		So(runCheck("mongodb/bin/mongod"), ShouldBeFalse)
	})
}
