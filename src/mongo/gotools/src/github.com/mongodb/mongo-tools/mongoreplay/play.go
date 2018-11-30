// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package mongoreplay

import (
	"fmt"
	"io"
	"time"

	mgo "github.com/10gen/llmgo"
)

// PlayCommand stores settings for the mongoreplay 'play' subcommand
type PlayCommand struct {
	GlobalOpts *Options `no-flag:"true"`
	StatOptions
	PlaybackFile string  `description:"path to the playback file to play from" short:"p" long:"playback-file" required:"yes"`
	Speed        float64 `description:"multiplier for playback speed (1.0 = real-time, .5 = half-speed, 3.0 = triple-speed, etc.)" long:"speed" default:"1.0"`
	URL          string  `short:"h" long:"host" description:"Location of the host to play back against" default:"mongodb://localhost:27017"`
	Repeat       int     `long:"repeat" description:"Number of times to play the playback file" default:"1"`
	QueueTime    int     `long:"queueTime" description:"don't queue ops much further in the future than this number of seconds" default:"15"`
	NoPreprocess bool    `long:"no-preprocess" description:"don't preprocess the input file to premap data such as mongo cursorIDs"`
	Gzip         bool    `long:"gzip" description:"decompress gzipped input"`
	Collect      string  `long:"collect" description:"Stat collection format; 'format' option uses the --format string" choice:"json" choice:"format" choice:"none" default:"none"`
	FullSpeed    bool    `long:"fullSpeed" description:"run the playback as fast as possible"`
}

const queueGranularity = 1000

// ValidateParams validates the settings described in the PlayCommand struct.
func (play *PlayCommand) ValidateParams(args []string) error {
	switch {
	case len(args) > 0:
		return fmt.Errorf("unknown argument: %s", args[0])
	case play.Speed <= 0:
		return fmt.Errorf("Invalid setting for --speed: '%v'", play.Speed)
	case play.Repeat < 1:
		return fmt.Errorf("Invalid setting for --repeat: '%v', value must be >=1", play.Repeat)
	}
	return nil
}

// Execute runs the program for the 'play' subcommand
func (play *PlayCommand) Execute(args []string) error {
	err := play.ValidateParams(args)
	if err != nil {
		return err
	}
	play.GlobalOpts.SetLogging()

	statColl, err := newStatCollector(play.StatOptions, play.Collect, true, true)
	if err != nil {
		return err
	}

	if play.FullSpeed {
		userInfoLogger.Logvf(Always, "Doing playback at full speed")
	} else {
		userInfoLogger.Logvf(Always, "Doing playback at %.2fx speed", play.Speed)
	}

	playbackFileReader, err := NewPlaybackFileReader(play.PlaybackFile, play.Gzip)
	if err != nil {
		return err
	}

	session, err := mgo.Dial(play.URL)
	if err != nil {
		return err
	}
	session.SetSocketTimeout(0)

	context := NewExecutionContext(statColl, session, &ExecutionOptions{fullSpeed: play.FullSpeed,
		driverOpsFiltered: playbackFileReader.metadata.DriverOpsFiltered})

	session.SetPoolLimit(-1)

	var opChan <-chan *RecordedOp
	var errChan <-chan error

	if !play.NoPreprocess {
		opChan, errChan = playbackFileReader.OpChan(1)

		preprocessMap, err := newPreprocessCursorManager(opChan)

		if err != nil {
			return fmt.Errorf("PreprocessMap: %v", err)
		}

		err = <-errChan
		if err != io.EOF {
			return fmt.Errorf("OpChan: %v", err)
		}

		_, err = playbackFileReader.Seek(0, 0)
		if err != nil {
			return err
		}
		context.CursorIDMap = preprocessMap
	}

	opChan, errChan = playbackFileReader.OpChan(play.Repeat)

	if err := Play(context, opChan, play.Speed, play.Repeat, play.QueueTime); err != nil {
		userInfoLogger.Logvf(Always, "Play: %v\n", err)
	}

	//handle the error from the errchan
	err = <-errChan
	if err != nil && err != io.EOF {
		userInfoLogger.Logvf(Always, "OpChan: %v", err)
	}
	return nil
}

// Play is responsible for playing ops from a RecordedOp channel to the session.
func Play(context *ExecutionContext,
	opChan <-chan *RecordedOp,
	speed float64,
	repeat int,
	queueTime int) error {

	connectionChans := make(map[int64]chan<- *RecordedOp)
	var playbackStartTime, recordingStartTime time.Time
	var connectionID int64
	var opCounter int
	for op := range opChan {
		opCounter++
		if op.Seen.IsZero() {
			return fmt.Errorf("Can't play operation found with zero-timestamp: %#v", op)
		}
		if recordingStartTime.IsZero() {
			recordingStartTime = op.Seen.Time
			playbackStartTime = time.Now()
		}

		// opDelta is the difference in time between when the file's recording
		// began and and when this particular op is played. For the first
		// operation in the playback, it's 0.
		opDelta := op.Seen.Sub(recordingStartTime)

		// Adjust the opDelta for playback by dividing it by playback speed setting;
		// e.g. 2x speed means the delta is half as long.
		scaledDelta := float64(opDelta) / (speed)
		op.PlayAt = &PreciseTime{playbackStartTime.Add(time.Duration(int64(scaledDelta)))}

		// Every queueGranularity ops make sure that we're no more then
		// QueueTime seconds ahead Which should mean that the maximum that we're
		// ever ahead is QueueTime seconds of ops + queueGranularity more ops.
		// This is so that when we're at QueueTime ahead in the playback file we
		// don't sleep after every read, and generally read and queue
		// queueGranularity number of ops at a time and then sleep until the
		// last read op is QueueTime ahead.
		if !context.fullSpeed {
			if opCounter%queueGranularity == 0 {
				toolDebugLogger.Logvf(DebugHigh, "Waiting to prevent excess buffering with opCounter: %v", opCounter)
				time.Sleep(op.PlayAt.Add(time.Duration(-queueTime) * time.Second).Sub(time.Now()))
			}
		}

		connectionChan, ok := connectionChans[op.SeenConnectionNum]
		if !ok {
			connectionID++
			connectionChan = context.newExecutionConnection(op.PlayAt.Time, connectionID)
			connectionChans[op.SeenConnectionNum] = connectionChan
		}
		if op.EOF {
			userInfoLogger.Logv(DebugLow, "EOF Seen in playback")
			close(connectionChan)
			delete(connectionChans, op.SeenConnectionNum)
		} else {
			connectionChan <- op
		}
	}
	for connectionNum, connectionChan := range connectionChans {
		close(connectionChan)
		delete(connectionChans, connectionNum)
	}
	toolDebugLogger.Logvf(Info, "Waiting for connections to finish")
	context.ConnectionChansWaitGroup.Wait()

	context.StatCollector.Close()
	toolDebugLogger.Logvf(Always, "%v ops played back in %v seconds over %v connections", opCounter, time.Now().Sub(playbackStartTime), connectionID)
	if repeat > 1 {
		toolDebugLogger.Logvf(Always, "%v ops per generation for %v generations", opCounter/repeat, repeat)
	}
	return nil
}
