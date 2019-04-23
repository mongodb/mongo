// Copyright (C) MongoDB, Inc. 2017-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package benchmark

import (
	"fmt"
	"time"

	"github.com/montanaflynn/stats"
)

type BenchResult struct {
	Name       string
	Trials     int
	Duration   time.Duration
	Raw        []Result
	DataSize   int
	Operations int
	hasErrors  *bool
}

func (r *BenchResult) EvergreenPerfFormat() ([]interface{}, error) {
	timings := r.timings()

	median, err := stats.Median(timings)
	if err != nil {
		return nil, err
	}

	min, err := stats.Min(timings)
	if err != nil {
		return nil, err
	}

	max, err := stats.Max(timings)
	if err != nil {
		return nil, err
	}

	out := []interface{}{
		map[string]interface{}{
			"name": r.Name + "-throughput",
			"results": map[string]interface{}{
				"1": map[string]interface{}{
					"seconds":        r.Duration.Round(time.Millisecond).Seconds(),
					"ops_per_second": r.getThroughput(median),
					"ops_per_second_values": []float64{
						r.getThroughput(min),
						r.getThroughput(max),
					},
				},
			},
		},
	}

	if r.DataSize > 0 {
		out = append(out, interface{}(map[string]interface{}{
			"name": r.Name + "-MB-adjusted",
			"results": map[string]interface{}{
				"1": map[string]interface{}{
					"seconds":        r.Duration.Round(time.Millisecond).Seconds(),
					"ops_per_second": r.adjustResults(median),
					"ops_per_second_values": []float64{
						r.adjustResults(min),
						r.adjustResults(max),
					},
				},
			},
		}))
	}

	return out, nil
}

func (r *BenchResult) timings() []float64 {
	out := []float64{}
	for _, r := range r.Raw {
		out = append(out, r.Duration.Seconds())
	}
	return out
}

func (r *BenchResult) totalDuration() time.Duration {
	var out time.Duration
	for _, trial := range r.Raw {
		out += trial.Duration
	}
	return out
}

func (r *BenchResult) adjustResults(data float64) float64 { return float64(r.DataSize) / data }
func (r *BenchResult) getThroughput(data float64) float64 { return float64(r.Operations) / data }
func (r *BenchResult) roundedRuntime() time.Duration      { return roundDurationMS(r.Duration) }

func (r *BenchResult) String() string {
	return fmt.Sprintf("name=%s, trials=%d, secs=%s", r.Name, r.Trials, r.Duration)
}

func (r *BenchResult) HasErrors() bool {
	if r.hasErrors == nil {
		var val bool
		for _, res := range r.Raw {
			if res.Error != nil {
				val = true
				break
			}
		}
		r.hasErrors = &val
	}

	return *r.hasErrors
}

func (r *BenchResult) errReport() []string {
	errs := []string{}
	for _, res := range r.Raw {
		if res.Error != nil {
			errs = append(errs, res.Error.Error())
		}
	}
	return errs
}

type Result struct {
	Duration   time.Duration
	Iterations int
	Error      error
}

func roundDurationMS(d time.Duration) time.Duration {
	rounded := d.Round(time.Millisecond)
	if rounded == 1<<63-1 {
		return 0
	}
	return rounded
}
