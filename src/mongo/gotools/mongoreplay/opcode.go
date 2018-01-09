// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package mongoreplay

import "fmt"

// OpCode allow identifying the type of operation:
// http://docs.mongodb.org/meta-driver/latest/legacy/mongodb-wire-protocol/#request-opcodes
type OpCode int32

// String returns a human readable representation of the OpCode.
func (c OpCode) String() string {
	switch c {
	case OpCodeReply:
		return "reply"
	case OpCodeMessage:
		return "message"
	case OpCodeUpdate:
		return "update"
	case OpCodeInsert:
		return "insert"
	case OpCodeReserved:
		return "reserved"
	case OpCodeQuery:
		return "query"
	case OpCodeGetMore:
		return "get_more"
	case OpCodeDelete:
		return "delete"
	case OpCodeKillCursors:
		return "kill_cursors"
	case OpCodeCommand:
		return "command"
	case OpCodeCommandReply:
		return "command_reply"
	default:
		return fmt.Sprintf("UNKNOWN(%d)", c)
	}
}

// The full set of known request op codes:
// http://docs.mongodb.org/meta-driver/latest/legacy/mongodb-wire-protocol/#request-opcodes
const (
	OpCodeReply        = OpCode(1)
	OpCodeUpdate       = OpCode(2001)
	OpCodeInsert       = OpCode(2002)
	OpCodeReserved     = OpCode(2003)
	OpCodeQuery        = OpCode(2004)
	OpCodeGetMore      = OpCode(2005)
	OpCodeDelete       = OpCode(2006)
	OpCodeKillCursors  = OpCode(2007)
	OpCodeCommand      = OpCode(2010)
	OpCodeCommandReply = OpCode(2011)
	OpCodeCompressed   = OpCode(2012)
	OpCodeMessage      = OpCode(2013)
)
