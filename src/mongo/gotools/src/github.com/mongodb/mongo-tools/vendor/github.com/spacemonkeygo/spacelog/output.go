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
	"bytes"
	"fmt"
	"io"
	"log"
	"os"
	"sync"
)

type TextOutput interface {
	Output(LogLevel, []byte)
}

// WriterOutput is an io.Writer wrapper that matches the TextOutput interface
type WriterOutput struct {
	w io.Writer
}

// NewWriterOutput returns a TextOutput that writes messages to an io.Writer
func NewWriterOutput(w io.Writer) *WriterOutput {
	return &WriterOutput{w: w}
}

func (o *WriterOutput) Output(_ LogLevel, message []byte) {
	o.w.Write(append(bytes.TrimRight(message, "\r\n"), platformNewline...))
}

// StdlibOutput is a TextOutput that simply writes to the default Go stdlib
// logging system. It is the default. If you configure the Go stdlib to write
// to spacelog, make sure to provide a new TextOutput to your logging
// collection
type StdlibOutput struct{}

func (*StdlibOutput) Output(_ LogLevel, message []byte) {
	log.Print(string(message))
}

type bufferMsg struct {
	level   LogLevel
	message []byte
}

// BufferedOutput uses a channel to synchronize writes to a wrapped TextOutput
// and allows for buffering a limited amount of log events.
type BufferedOutput struct {
	o          TextOutput
	c          chan bufferMsg
	running    sync.Mutex
	close_once sync.Once
}

// NewBufferedOutput returns a BufferedOutput wrapping output with a buffer
// size of buffer.
func NewBufferedOutput(output TextOutput, buffer int) *BufferedOutput {
	if buffer < 0 {
		buffer = 0
	}
	b := &BufferedOutput{
		o: output,
		c: make(chan bufferMsg, buffer)}
	go b.process()
	return b
}

// Close shuts down the BufferedOutput's processing
func (b *BufferedOutput) Close() {
	b.close_once.Do(func() {
		close(b.c)
	})
	b.running.Lock()
	b.running.Unlock()
}

func (b *BufferedOutput) Output(level LogLevel, message []byte) {
	b.c <- bufferMsg{level: level, message: message}
}

func (b *BufferedOutput) process() {
	b.running.Lock()
	defer b.running.Unlock()
	for {
		msg, open := <-b.c
		if !open {
			break
		}
		b.o.Output(msg.level, msg.message)
	}
}

// A TextOutput object that also implements HupHandlingTextOutput may have its
// OnHup() method called when an administrative signal is sent to this process.
type HupHandlingTextOutput interface {
	TextOutput
	OnHup()
}

// FileWriterOutput is like WriterOutput with a plain file handle, but it
// knows how to reopen the file (or try to reopen it) if it hasn't been able
// to open the file previously, or if an appropriate signal has been received.
type FileWriterOutput struct {
	*WriterOutput
	path string
}

// Creates a new FileWriterOutput object. This is the only case where an
// error opening the file will be reported to the caller; if we try to
// reopen it later and the reopen fails, we'll just keep trying until it
// works.
func NewFileWriterOutput(path string) (*FileWriterOutput, error) {
	fo := &FileWriterOutput{path: path}
	fh, err := fo.openFile()
	if err != nil {
		return nil, err
	}
	fo.WriterOutput = NewWriterOutput(fh)
	return fo, nil
}

// Try to open the file with the path associated with this object.
func (fo *FileWriterOutput) openFile() (*os.File, error) {
	return os.OpenFile(fo.path, os.O_WRONLY|os.O_CREATE|os.O_APPEND, 0644)
}

// Try to communicate a message without using our log file. In all likelihood,
// stderr is closed or redirected to /dev/null, but at least we can try
// writing there. In the very worst case, if an admin attaches a ptrace to
// this process, it will be more clear what the problem is.
func (fo *FileWriterOutput) fallbackLog(tmpl string, args ...interface{}) {
	fmt.Fprintf(os.Stderr, tmpl, args...)
}

// Output a log line by writing it to the file. If the file has been
// released, try to open it again. If that fails, cry for a little
// while, then throw away the message and carry on.
func (fo *FileWriterOutput) Output(ll LogLevel, message []byte) {
	if fo.WriterOutput == nil {
		fh, err := fo.openFile()
		if err != nil {
			fo.fallbackLog("Could not open %#v: %s", fo.path, err)
			return
		}
		fo.WriterOutput = NewWriterOutput(fh)
	}
	fo.WriterOutput.Output(ll, message)
}

// Throw away any references/handles to the output file. This probably
// means the admin wants to rotate the file out and have this process
// open a new one. Close the underlying io.Writer if that is a thing
// that it knows how to do.
func (fo *FileWriterOutput) OnHup() {
	if fo.WriterOutput != nil {
		wc, ok := fo.WriterOutput.w.(io.Closer)
		if ok {
			err := wc.Close()
			if err != nil {
				fo.fallbackLog("Closing %#v failed: %s", fo.path, err)
			}
		}
		fo.WriterOutput = nil
	}
}
