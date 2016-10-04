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

/*
Package spacelog is a collection of interface lego bricks designed to help you
build a flexible logging system.

spacelog is loosely inspired by the Python logging library.

The basic interaction is between a Logger and a Handler. A Logger is
what the programmer typically interacts with for creating log messages. A
Logger will be at a given log level, and if log messages can clear that
specific logger's log level filter, they will be passed off to the Handler.

Loggers are instantiated from GetLogger and GetLoggerNamed.

A Handler is a very generic interface for handling log events. You can provide
your own Handler for doing structured JSON output or colorized output or
countless other things.

Provided are a simple TextHandler with a variety of log event templates and
TextOutput sinks, such as io.Writer, Syslog, and so forth.

Make sure to see the source of the setup subpackage for an example of easy and
configurable logging setup at process start:
  http://godoc.org/github.com/spacemonkeygo/spacelog/setup
*/
package spacelog
