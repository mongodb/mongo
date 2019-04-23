// Copyright (C) MongoDB, Inc. 2017-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package benchmark

import (
	"context"
	"encoding/json"
	"flag"
	"fmt"
	"io/ioutil"
	"os"
)

func DriverBenchmarkMain() int {
	var hasErrors bool
	var outputFileName string
	flag.StringVar(&outputFileName, "output", "perf.json", "path to write the 'perf.json' file")
	flag.Parse()

	ctx := context.Background()
	output := []interface{}{}
	for _, res := range runDriverCases(ctx) {
		if res.HasErrors() {
			hasErrors = true
		}

		evg, err := res.EvergreenPerfFormat()
		if err != nil {
			hasErrors = true
			continue
		}

		output = append(output, evg...)
	}

	evgOutput, err := json.MarshalIndent(map[string]interface{}{"results": output}, "", "   ")
	if err != nil {
		return 1
	}
	evgOutput = append(evgOutput, []byte("\n")...)

	if outputFileName == "" {
		fmt.Println(string(evgOutput))
	} else if err := ioutil.WriteFile(outputFileName, evgOutput, 0644); err != nil {
		fmt.Fprintf(os.Stderr, "problem writing file '%s': %s", outputFileName, err.Error())
		return 1
	}

	if hasErrors {
		return 1
	}

	return 0
}

func runDriverCases(ctx context.Context) []*BenchResult {
	cases := getAllCases()

	results := []*BenchResult{}
	for _, bc := range cases {
		results = append(results, bc.Run(ctx))
	}

	return results
}
