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

var goodOpCode = map[int32]bool{
	1:    true, //OP_REPLY          Reply to a client request. responseTo is set.
	2001: true, //OP_UPDATE         Update document.
	2002: true, //OP_INSERT         Insert new document.
	2003: true, //RESERVED          Formerly used for OP_GET_BY_OID.
	2004: true, //OP_QUERY          Query a collection.
	2005: true, //OP_GET_MORE       Get more data from a query. See Cursors.
	2006: true, //OP_DELETE         Delete documents.
	2007: true, //OP_KILL_CURSORS   Notify database that the client has finished with the cursor.
	2010: true, //OP_COMMAND        A new wire protocol message representing a command request
	2011: true, //OP_COMMANDREPLY   A new wire protocol message representing a command
	2012: true, //OP_COMPRESSED     Compressed op
	2013: true, //OP_MSG			New command/reply type
}
