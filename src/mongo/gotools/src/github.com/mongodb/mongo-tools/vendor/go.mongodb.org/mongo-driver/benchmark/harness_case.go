// Copyright (C) MongoDB, Inc. 2017-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package benchmark

import (
	"context"
	"fmt"
	"path/filepath"
	"reflect"
	"runtime"
	"strings"
	"time"
)

type CaseDefinition struct {
	Bench              BenchCase
	Count              int
	Size               int
	RequiredIterations int
	Runtime            time.Duration

	cumulativeRuntime time.Duration
	elapsed           time.Duration
	startAt           time.Time
	isRunning         bool
}

// TimerManager is a subset of the testing.B tool, used to manage
// setup code.
type TimerManager interface {
	ResetTimer()
	StartTimer()
	StopTimer()
}

func (c *CaseDefinition) ResetTimer() {
	c.startAt = time.Now()
	c.elapsed = 0
	c.isRunning = true
}

func (c *CaseDefinition) StartTimer() {
	c.startAt = time.Now()
	c.isRunning = true
}

func (c *CaseDefinition) StopTimer() {
	if !c.isRunning {
		return
	}
	c.elapsed += time.Since(c.startAt)
	c.isRunning = false
}

func (c *CaseDefinition) roundedRuntime() time.Duration {
	return roundDurationMS(c.Runtime)
}

func (c *CaseDefinition) Run(ctx context.Context) *BenchResult {
	out := &BenchResult{
		Trials:     1,
		DataSize:   c.Size,
		Name:       c.Name(),
		Operations: c.Count,
	}
	var cancel context.CancelFunc
	ctx, cancel = context.WithTimeout(ctx, 2*ExecutionTimeout)
	defer cancel()

	fmt.Println("=== RUN", out.Name)
	if c.RequiredIterations == 0 {
		c.RequiredIterations = MinIterations
	}

benchRepeat:
	for {
		if ctx.Err() != nil {
			break
		}
		if c.cumulativeRuntime >= c.Runtime {
			if out.Trials >= c.RequiredIterations {
				break
			} else if c.cumulativeRuntime >= ExecutionTimeout {
				break
			}
		}

		res := Result{
			Iterations: c.Count,
		}

		c.StartTimer()
		res.Error = c.Bench(ctx, c, c.Count)
		c.StopTimer()
		res.Duration = c.elapsed
		c.cumulativeRuntime += res.Duration

		switch res.Error {
		case context.DeadlineExceeded:
			break benchRepeat
		case context.Canceled:
			break benchRepeat
		case nil:
			out.Trials++
			c.elapsed = 0
			out.Raw = append(out.Raw, res)
		default:
			continue
		}

	}

	out.Duration = out.totalDuration()
	fmt.Printf("    --- REPORT: count=%d trials=%d requiredTrials=%d runtime=%s\n",
		c.Count, out.Trials, c.RequiredIterations, c.Runtime)
	if out.HasErrors() {
		fmt.Printf("    --- ERRORS: %s\n", strings.Join(out.errReport(), "\n       "))
		fmt.Printf("--- FAIL: %s (%s)\n", out.Name, out.roundedRuntime())
	} else {
		fmt.Printf("--- PASS: %s (%s)\n", out.Name, out.roundedRuntime())
	}

	return out

}

func (c *CaseDefinition) String() string {
	return fmt.Sprintf("name=%s, count=%d, runtime=%s timeout=%s",
		c.Name(), c.Count, c.Runtime, ExecutionTimeout)
}

func (c *CaseDefinition) Name() string { return getName(c.Bench) }
func getName(i interface{}) string {
	n := runtime.FuncForPC(reflect.ValueOf(i).Pointer()).Name()
	parts := strings.Split(n, ".")
	if len(parts) > 1 {
		return parts[len(parts)-1]
	}

	return n

}

func getProjectRoot() string { return filepath.Dir(getDirectoryOfFile()) }

func getDirectoryOfFile() string {
	_, file, _, _ := runtime.Caller(1)

	return filepath.Dir(file)
}
