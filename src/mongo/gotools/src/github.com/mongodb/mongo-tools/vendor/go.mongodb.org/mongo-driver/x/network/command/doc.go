// Copyright (C) MongoDB, Inc. 2017-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

// Package command contains abstractions for operations that can be performed against a MongoDB
// deployment. The types in this package are meant to provide a general set of commands that a
// user can run against a MongoDB database without knowing the version of the database.
//
// Each type consists of two levels of interaction. The lowest level are the Encode and Decode
// methods. These are meant to be symmetric eventually, but currently only support the driver
// side of commands. The higher level is the RoundTrip method. This only makes sense from the
// driver side of commands and this method handles the encoding of the request and decoding of
// the response using the given wiremessage.ReadWriter.
package command
