// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package mongoreplay

import (
	"fmt"
	"io"
	"os"
	"sync"
	"time"
)

// FilterCommand stores settings for the mongoreplay 'filter' subcommand
type FilterCommand struct {
	GlobalOpts      *Options `no-flag:"true"`
	PlaybackFile    string   `description:"path to the playback file to read from" short:"p" long:"playback-file" required:"yes"`
	OutFile         string   `description:"path to the output file to write to" short:"o" long:"outputFile"`
	SplitFilePrefix string   `description:"prefix file name to use for the output files being written when splitting traffic" long:"outfilePrefix"`
	StartTime       string   `description:"ISO 8601 timestamp to remove all operations before" long:"startAt"`
	Duration        string   `description:"truncate the end of the file after a certain duration from the time of the first seen operation" long:"duration"`
	Split           int      `description:"split the traffic into n files with roughly equal numbers of connecitons in each" default:"1" long:"split"`
	RemoveDriverOps bool     `description:"remove driver issued operations from the playback" long:"removeDriverOps"`
	Gzip            bool     `long:"gzip" description:"decompress gzipped input"`

	duration  time.Duration
	startTime time.Time
}

type skipConfig struct {
	firstOpTime, lastOpTime *time.Time
	truncateDuration        *time.Duration
	removeDriverOps         bool
}

func newSkipConfig(removeDriverOps bool, startTime time.Time, truncateDuration time.Duration) *skipConfig {
	skipConf := &skipConfig{
		removeDriverOps: removeDriverOps,
	}
	if !startTime.IsZero() {
		skipConf.firstOpTime = &startTime
	}
	if truncateDuration.Nanoseconds() != 0 {
		skipConf.truncateDuration = &truncateDuration
	}
	return skipConf
}

// Execute runs the program for the 'filter' subcommand
func (filter *FilterCommand) Execute(args []string) error {
	err := filter.ValidateParams(args)
	if err != nil {
		return err
	}
	filter.GlobalOpts.SetLogging()

	playbackFileReader, err := NewPlaybackFileReader(filter.PlaybackFile, filter.Gzip)
	if err != nil {
		return err
	}
	opChan, errChan := playbackFileReader.OpChan(1)

	driverOpsFiltered := filter.RemoveDriverOps || playbackFileReader.metadata.DriverOpsFiltered

	outfiles := make([]*PlaybackFileWriter, filter.Split)
	if filter.Split == 1 {
		playbackWriter, err := NewPlaybackFileWriter(filter.OutFile, driverOpsFiltered,
			filter.Gzip)
		if err != nil {
			return err
		}
		outfiles[0] = playbackWriter
	} else {
		for i := 0; i < filter.Split; i++ {
			playbackWriter, err := NewPlaybackFileWriter(
				fmt.Sprintf("%s%02d.playback", filter.SplitFilePrefix, i), driverOpsFiltered,
				filter.Gzip)
			if err != nil {
				return err
			}
			outfiles[i] = playbackWriter
			defer playbackWriter.Close()
		}
	}

	skipConf := newSkipConfig(filter.RemoveDriverOps, filter.startTime, filter.duration)

	if err := Filter(opChan, outfiles, skipConf); err != nil {
		userInfoLogger.Logvf(Always, "Filter: %v\n", err)
	}

	//handle the error from the errchan
	err = <-errChan
	if err != nil && err != io.EOF {
		userInfoLogger.Logvf(Always, "OpChan: %v", err)
	}
	return nil
}

func Filter(opChan <-chan *RecordedOp,
	outfiles []*PlaybackFileWriter,
	skipConf *skipConfig) error {

	opWriters := make([]chan<- *RecordedOp, len(outfiles))
	errChan := make(chan error)
	wg := &sync.WaitGroup{}

	for i := range outfiles {
		opWriters[i] = newParallelPlaybackWriter(outfiles[i], errChan, wg)
	}

	for op := range opChan {
		shouldSkip, err := skipConf.shouldFilterOp(op)
		if err != nil {
			return err
		}
		if shouldSkip {
			continue
		}
		fileNum := op.SeenConnectionNum % int64(len(outfiles))
		opWriters[fileNum] <- op
	}

	for _, opWriter := range opWriters {
		close(opWriter)
	}
	wg.Wait()
	close(errChan)

	var hasError bool
	for err := range errChan {
		hasError = true
		userInfoLogger.Logvf(Always, "error: %s", err)
	}
	if hasError {
		return fmt.Errorf("errors encountered while running filter")
	}

	return nil
}

func newParallelPlaybackWriter(outfile *PlaybackFileWriter,
	errChan chan<- error, wg *sync.WaitGroup) chan<- *RecordedOp {
	var didWriteOp bool

	inputOpChan := make(chan *RecordedOp, 1000)
	wg.Add(1)
	go func() {
		defer wg.Done()
		for op := range inputOpChan {
			err := bsonToWriter(outfile, op)
			if err != nil {
				errChan <- err
				return
			}
			didWriteOp = true
		}
		if !didWriteOp {
			userInfoLogger.Logvf(Always, "no connections written to file %s, removing", outfile.fname)
			err := os.Remove(outfile.fname)
			if err != nil {
				errChan <- err
				return
			}
		}
	}()
	return inputOpChan
}

func (filter *FilterCommand) ValidateParams(args []string) error {
	switch {
	case filter.Split < 1:
		return fmt.Errorf("must be a positive number of files to split into")
	case filter.Split > 1 && filter.SplitFilePrefix == "":
		return fmt.Errorf("must specify a filename prefix when splitting traffic")
	case filter.Split > 1 && filter.OutFile != "":
		return fmt.Errorf("must not specify an output file name when splitting traffic" +
			"instead only specify a file name prefix")
	case filter.Split == 1 && filter.OutFile == "":
		return fmt.Errorf("must specify an output file")
	}

	if filter.StartTime != "" {
		t, err := time.Parse(time.RFC3339, filter.StartTime)
		if err != nil {
			return fmt.Errorf("error parsing start time argument: %v", err)
		}
		filter.startTime = t
	}

	if filter.Duration != "" {
		d, err := time.ParseDuration(filter.Duration)
		if err != nil {
			return fmt.Errorf("error parsing duration argument: %v", err)
		}
		filter.duration = d
	}

	return nil
}

func (sc *skipConfig) shouldFilterOp(op *RecordedOp) (bool, error) {
	// Skip ops until the target first time if specified
	if sc.firstOpTime != nil && op.Seen.Before(*sc.firstOpTime) {
		return true, nil
	}

	// Initialize target last op time based on first op kept after initial truncation
	if sc.lastOpTime == nil && sc.truncateDuration != nil {
		lastOpTime := op.Seen.Add(*sc.truncateDuration)
		sc.lastOpTime = &lastOpTime
	}

	// Skip ops after a target last time if specified
	if sc.lastOpTime != nil && op.Seen.After(*sc.lastOpTime) {
		return true, nil
	}

	// Check if driver op
	if sc.removeDriverOps {
		parsedOp, err := op.RawOp.Parse()
		if err != nil {
			return true, err
		}
		return IsDriverOp(parsedOp), nil
	}

	return false, nil
}
