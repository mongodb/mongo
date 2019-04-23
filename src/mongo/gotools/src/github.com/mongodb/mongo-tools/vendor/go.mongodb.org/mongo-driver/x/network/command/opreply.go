// Copyright (C) MongoDB, Inc. 2017-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package command

import (
	"go.mongodb.org/mongo-driver/bson"
	"go.mongodb.org/mongo-driver/x/network/wiremessage"
)

// decodeCommandOpReply handles decoding the OP_REPLY response to an OP_QUERY
// command.
func decodeCommandOpReply(reply wiremessage.Reply) (bson.Raw, error) {
	if reply.NumberReturned == 0 {
		return nil, ErrNoDocCommandResponse
	}
	if reply.NumberReturned > 1 {
		return nil, ErrMultiDocCommandResponse
	}
	if len(reply.Documents) != 1 {
		return nil, NewCommandResponseError("malformed OP_REPLY: NumberReturned does not match number of documents returned", nil)
	}
	rdr := reply.Documents[0]
	err := rdr.Validate()
	if err != nil {
		return nil, NewCommandResponseError("malformed OP_REPLY: invalid document", err)
	}
	if reply.ResponseFlags&wiremessage.QueryFailure == wiremessage.QueryFailure {
		return nil, QueryFailureError{
			Message:  "command failure",
			Response: reply.Documents[0],
		}
	}

	err = extractError(rdr)
	if err != nil {
		return nil, err
	}
	return rdr, nil
}
