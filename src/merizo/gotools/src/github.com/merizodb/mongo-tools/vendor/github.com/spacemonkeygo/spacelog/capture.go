// Copyright (C) 2014 Space Monkey, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

package spacelog

import (
	"fmt"
	"os"
	"os/exec"
)

// CaptureOutputToFile opens a filehandle using the given path, then calls
// CaptureOutputToFd on the associated filehandle.
func CaptureOutputToFile(path string) error {
	fh, err := os.OpenFile(path, os.O_RDWR|os.O_CREATE, 0644)
	if err != nil {
		return err
	}
	defer fh.Close()
	return CaptureOutputToFd(int(fh.Fd()))
}

// CaptureOutputToProcess starts a process and using CaptureOutputToFd,
// redirects stdout and stderr to the subprocess' stdin.
// CaptureOutputToProcess expects the subcommand to last the lifetime of the
// process, and if the subprocess dies, will panic.
func CaptureOutputToProcess(command string, args ...string) error {
	cmd := exec.Command(command, args...)
	out, err := cmd.StdinPipe()
	if err != nil {
		return err
	}
	defer out.Close()
	type fder interface {
		Fd() uintptr
	}
	out_fder, ok := out.(fder)
	if !ok {
		return fmt.Errorf("unable to get underlying pipe")
	}
	err = CaptureOutputToFd(int(out_fder.Fd()))
	if err != nil {
		return err
	}
	err = cmd.Start()
	if err != nil {
		return err
	}
	go func() {
		err := cmd.Wait()
		if err != nil {
			panic(fmt.Errorf("captured output process died! %s", err))
		}
	}()
	return nil
}
