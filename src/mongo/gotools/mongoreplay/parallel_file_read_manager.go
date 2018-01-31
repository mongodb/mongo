// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package mongoreplay

import (
	"io"
	"sync"

	"github.com/10gen/llmgo/bson"
)

type parallelFileReadManager struct {
	fileReadErr                     error
	parseJobsChan                   chan *parseJob
	workerResultManagers            []workerResultManager
	stopChan                        chan struct{}
	currentWorkerResultManagerIndex int
}

type parseJob struct {
	rawDoc              []byte
	workerResultManager workerResultManager
}

type workerResultManager struct {
	resultChan chan *recordedOpResult
	available  chan struct{}
}

type recordedOpResult struct {
	recordedOp *RecordedOp
	err        error
}

func (pm *parallelFileReadManager) runFileReader(numWorkers int, reader io.Reader) {
	currentWorkerResultManagerIndex := 0
	go func() {
		defer close(pm.parseJobsChan)
		for {
			currentWorkerResultManager := pm.workerResultManagers[currentWorkerResultManagerIndex]
			currentWorkerResultManagerIndex = (currentWorkerResultManagerIndex + 1) % numWorkers
			nextDoc, err := ReadDocument(reader)
			if err != nil {
				if err == io.EOF {
					return
				}
				pm.fileReadErr = err
				close(pm.stopChan)
				return
			}

			<-currentWorkerResultManager.available
			pm.parseJobsChan <- &parseJob{
				rawDoc:              nextDoc,
				workerResultManager: currentWorkerResultManager,
			}
		}
	}()
}

func (pm *parallelFileReadManager) runParsePool(numWorkers int) {
	wg := &sync.WaitGroup{}
	for i := 0; i < numWorkers; i++ {
		wg.Add(1)
		go runParseWorker(pm.parseJobsChan, wg, pm.stopChan)
	}
	go func() {
		wg.Wait()
		for _, workerResultManager := range pm.workerResultManagers {
			close(workerResultManager.resultChan)
			close(workerResultManager.available)
		}
	}()
}

func runParseWorker(parseJobsChan chan *parseJob, wg *sync.WaitGroup, stop chan struct{}) {
	defer wg.Done()
	for parseJob := range parseJobsChan {
		doc := new(RecordedOp)
		err := bson.Unmarshal(parseJob.rawDoc, doc)

		result := &recordedOpResult{
			err:        err,
			recordedOp: doc,
		}

		select {
		case parseJob.workerResultManager.resultChan <- result:
			parseJob.workerResultManager.available <- struct{}{}
		case <-stop:
			return
		}
	}

}

// begin initiates all aspects of the parallelFileReadManager. begin sets up the
// channels that work will be communicated on, starts the goroutine that will
// read through the file, and spawns the pool of goroutines that will parse
// the file in parallel.
func (pm *parallelFileReadManager) begin(numWorkers int, reader io.Reader) {
	pm.workerResultManagers = make([]workerResultManager, numWorkers)
	for i := 0; i < numWorkers; i++ {
		pm.workerResultManagers[i] = workerResultManager{
			resultChan: make(chan *recordedOpResult),
			available:  make(chan struct{}, 1),
		}
		pm.workerResultManagers[i].available <- struct{}{}
	}

	pm.parseJobsChan = make(chan *parseJob, numWorkers)
	pm.stopChan = make(chan struct{})

	pm.runFileReader(numWorkers, reader)
	pm.runParsePool(numWorkers)
}

// next is the function to be called to fetch each document from the file reader.
// It returns the next document parsed from the input file. next is not safe to
// call from a multi-threaded context.
func (pm *parallelFileReadManager) next() (*RecordedOp, error) {
	currentWorkerResultManager := pm.workerResultManagers[pm.currentWorkerResultManagerIndex]
	recordedOpResult := <-currentWorkerResultManager.resultChan
	if recordedOpResult == nil {
		return nil, io.EOF
	}

	pm.currentWorkerResultManagerIndex = (pm.currentWorkerResultManagerIndex + 1) % len(pm.workerResultManagers)
	return recordedOpResult.recordedOp, recordedOpResult.err
}

func (pm *parallelFileReadManager) err() error {
	return pm.fileReadErr
}
