package mongostat

import (
	"github.com/mongodb/mongo-tools/common/testutil"
	. "github.com/smartystreets/goconvey/convey"
	"testing"
	"time"
)

func TestStatLine(t *testing.T) {
	testutil.VerifyTestType(t, testutil.UnitTestType)

	faultsOld := int64(10)
	faultsNew := int64(15)
	serverStatusOld := ServerStatus{
		SampleTime:     time.Now(),
		Host:           "localhost",
		Version:        "test-version",
		Process:        "mongod",
		Pid:            10,
		Uptime:         1000,
		UptimeMillis:   1000,
		UptimeEstimate: 1000,
		LocalTime:      time.Now(),
		Asserts:        map[string]int64{},
		BackgroundFlushing: &FlushStats{
			Flushes:      2,
			TotalMs:      100,
			AverageMs:    101,
			LastMs:       102,
			LastFinished: time.Now(),
		},
		ExtraInfo: &ExtraInfo{PageFaults: &faultsOld},
		Connections: &ConnectionStats{
			Current:      5,
			Available:    1024,
			TotalCreated: 2048,
		},
		Dur: &DurStats{
			Commits:            1,
			JournaledMB:        2,
			WriteToDataFilesMB: 4,
			Compression:        8,
			CommitsInWriteLock: 16,
			EarlyCommits:       32,
		},
		GlobalLock: &GlobalLockStats{
			TotalTime: 1024,
			LockTime:  2048,
			CurrentQueue: &QueueStats{
				Total:   5,
				Readers: 3,
				Writers: 2,
			},
			ActiveClients: &ClientStats{
				Total:   5,
				Readers: 3,
				Writers: 2,
			},
		},
		Locks: map[string]LockStats{
			".": {
				TimeLockedMicros:    ReadWriteLockTimes{Read: 2850999, Write: 1807873},
				TimeAcquiringMicros: ReadWriteLockTimes{Read: 1393322, Write: 246102},
			},
			"test": {
				TimeLockedMicros:    ReadWriteLockTimes{Read: 663190, Write: 379},
				TimeAcquiringMicros: ReadWriteLockTimes{Read: 200443, Write: 6},
			},
		},
		Network: &NetworkStats{
			BytesIn:     106484804,
			BytesOut:    52805308,
			NumRequests: 873667,
		},
		Opcounters: &OpcountStats{
			Insert:  872001,
			Query:   2877,
			Update:  399,
			Delete:  203,
			GetMore: 101,
			Command: 985,
		},
		OpcountersRepl: &OpcountStats{
			Insert:  1234,
			Query:   4567,
			Update:  89,
			Delete:  10,
			GetMore: 111,
			Command: 999,
		},
		Mem: &MemStats{
			Bits:              64,
			Resident:          53,
			Virtual:           35507,
			Supported:         true,
			Mapped:            16489,
			MappedWithJournal: 32978,
		},
	}

	serverStatusNew := ServerStatus{
		SampleTime:     time.Now(),
		Host:           "localhost",
		Version:        "test-version",
		Process:        "mongod",
		Pid:            10,
		Uptime:         1000,
		UptimeMillis:   2000,
		UptimeEstimate: 1000,
		LocalTime:      time.Now(),
		Asserts:        map[string]int64{},
		BackgroundFlushing: &FlushStats{
			Flushes:      2,
			TotalMs:      100,
			AverageMs:    101,
			LastMs:       102,
			LastFinished: time.Now(),
		},
		ExtraInfo: &ExtraInfo{PageFaults: &faultsNew},
		Connections: &ConnectionStats{
			Current:      5,
			Available:    1024,
			TotalCreated: 2048,
		},
		Dur: &DurStats{
			Commits:            1,
			JournaledMB:        2,
			WriteToDataFilesMB: 4,
			Compression:        8,
			CommitsInWriteLock: 16,
			EarlyCommits:       32,
		},
		GlobalLock: &GlobalLockStats{
			TotalTime: 1024,
			LockTime:  2048,
			CurrentQueue: &QueueStats{
				Total:   5,
				Readers: 3,
				Writers: 2,
			},
			ActiveClients: &ClientStats{
				Total:   5,
				Readers: 4,
				Writers: 6,
			},
		},
		Locks: map[string]LockStats{
			".": {
				TimeLockedMicros:    ReadWriteLockTimes{Read: 2850999, Write: 1807873},
				TimeAcquiringMicros: ReadWriteLockTimes{Read: 1393322, Write: 246102},
			},
			"test": {
				TimeLockedMicros:    ReadWriteLockTimes{Read: 663190, Write: 500397},
				TimeAcquiringMicros: ReadWriteLockTimes{Read: 200443, Write: 6},
			},
		},
		Network: &NetworkStats{
			BytesIn:     106486804,
			BytesOut:    52808308,
			NumRequests: 873667,
		},
		Opcounters: &OpcountStats{
			Insert:  872011,
			Query:   2882,
			Update:  406,
			Delete:  205,
			GetMore: 104,
			Command: 1654,
		},
		OpcountersRepl: &OpcountStats{
			Insert:  1234,
			Query:   4567,
			Update:  89,
			Delete:  10,
			GetMore: 111,
			Command: 999,
		},
		Mem: &MemStats{
			Bits:              64,
			Resident:          53,
			Virtual:           35507,
			Supported:         true,
			Mapped:            16489,
			MappedWithJournal: 32978,
		},
	}

	Convey("StatsLine should accurately calculate opcounter diffs", t, func() {
		statsLine := NewStatLine(serverStatusOld, serverStatusNew, "", false, 1)
		So(statsLine.Insert, ShouldEqual, 10)
		So(statsLine.Query, ShouldEqual, 5)
		So(statsLine.Update, ShouldEqual, 7)
		So(statsLine.Delete, ShouldEqual, 2)
		So(statsLine.GetMore, ShouldEqual, 3)
		So(statsLine.Command, ShouldEqual, 669)

		So(statsLine.Faults, ShouldEqual, 5)
		So(statsLine.HighestLocked.DBName, ShouldEqual, "test")
		So(statsLine.HighestLocked.Percentage, ShouldAlmostEqual, 50.0)
		So(statsLine.QueuedReaders, ShouldEqual, 3)
		So(statsLine.QueuedWriters, ShouldEqual, 2)
		So(statsLine.ActiveReaders, ShouldEqual, 4)
		So(statsLine.ActiveWriters, ShouldEqual, 6)
		So(statsLine.NetIn, ShouldEqual, 2000)
		So(statsLine.NetOut, ShouldEqual, 3000)
		So(statsLine.NumConnections, ShouldEqual, 5)
	})

	Convey("StatsLine with non-default interval should calculate average diffs", t, func() {
		statsLine := NewStatLine(serverStatusOld, serverStatusNew, "", false, 3)
		// Opcounters and faults are averaged over sample period
		So(statsLine.Insert, ShouldEqual, 3)
		So(statsLine.Query, ShouldEqual, 1)
		So(statsLine.Update, ShouldEqual, 2)
		So(statsLine.Delete, ShouldEqual, 0)
		So(statsLine.GetMore, ShouldEqual, 1)
		So(statsLine.Command, ShouldEqual, 223)
		So(statsLine.Faults, ShouldEqual, 1)

		So(statsLine.HighestLocked.DBName, ShouldEqual, "test")
		So(statsLine.HighestLocked.Percentage, ShouldAlmostEqual, 50.0)
		So(statsLine.QueuedReaders, ShouldEqual, 3)
		So(statsLine.QueuedWriters, ShouldEqual, 2)
		So(statsLine.ActiveReaders, ShouldEqual, 4)
		So(statsLine.ActiveWriters, ShouldEqual, 6)
		// NetIn/Out is averaged over sample period
		So(statsLine.NetIn, ShouldEqual, 666)
		So(statsLine.NetOut, ShouldEqual, 1000)
		So(statsLine.NumConnections, ShouldEqual, 5)
	})
}
