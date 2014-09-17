package mongostat

import (
	. "github.com/smartystreets/goconvey/convey"
	"testing"
	"time"
)

func TestStatLine(t *testing.T) {
	faultsOld := 10
	faultsNew := 15
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
		Asserts:        map[string]int{},
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
		IndexCounter: &IndexCounterStats{
			Accesses:  11429830532,
			Hits:      11429794611,
			Misses:    0,
			Resets:    0,
			MissRatio: 0,
		},
		Locks: map[string]LockStats{
			".": {
				ReadWriteLockTimes{Read: 2850999, Write: 1807873}, //locked
				ReadWriteLockTimes{Read: 1393322, Write: 246102},  // acquiring
			},
			"test": {
				ReadWriteLockTimes{Read: 663190, Write: 379}, //locked
				ReadWriteLockTimes{Read: 200443, Write: 6},   // acquiring
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
		UptimeMillis:   1000,
		UptimeEstimate: 1000,
		LocalTime:      time.Now(),
		Asserts:        map[string]int{},
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
		IndexCounter: &IndexCounterStats{
			Accesses:  11429830532,
			Hits:      11429794611,
			Misses:    0,
			Resets:    0,
			MissRatio: 0,
		},
		Locks: map[string]LockStats{
			".": {
				ReadWriteLockTimes{Read: 2850999, Write: 1807873}, //locked
				ReadWriteLockTimes{Read: 1393322, Write: 246102},  // acquiring
			},
			"test": {
				ReadWriteLockTimes{Read: 663190, Write: 500397}, //locked
				ReadWriteLockTimes{Read: 200443, Write: 6},      // acquiring
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

	statsLine := NewStatLine(serverStatusOld, serverStatusNew, false)

	Convey("StatsLine should accurately calculate opcounter diffs", t, func() {
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

		//StatsLine should accurately calculate opcounter diffs ✔✔✔✔✔✔&mongostat.StatLine{Insert:10, Query:5, Update:7, Delete:2, GetMore:3, Command:669, Flushes:0, Mapped:16489, Virtual:35507, Resident:53, Faults:(*int)(0xc208000130), HighestLocked:mongostat.LockStatus{DBName:"test", Percentage:0, Global:false}, IndexMissPercent:0, QueuedReaders:3, QueuedWriters:2, ActiveReaders:3, ActiveWriters:2, NetIn:0, NetOut:0, NumConnections:5, Time:time.Time{sec:0, nsec:0x0, loc:(*time.Location)(nil)}}
	})
}
