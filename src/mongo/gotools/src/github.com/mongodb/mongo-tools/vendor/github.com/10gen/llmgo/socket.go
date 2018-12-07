// mgo - MongoDB driver for Go
//
// Copyright (c) 2010-2012 - Gustavo Niemeyer <gustavo@niemeyer.net>
//
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice, this
//    list of conditions and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
// ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
// ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
// (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
// LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
// ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

package mgo

import (
	"bytes"
	"errors"
	"fmt"
	"io"
	"net"
	"sync"
	"time"

	"github.com/10gen/llmgo/bson"
)

const (
	opInvalid      = 0
	opReply        = 1
	dbMsg          = 1000
	dbUpdate       = 2001
	dbInsert       = 2002
	dbQuery        = 2004
	dbGetMore      = 2005
	dbDelete       = 2006
	dbKillCursors  = 2007
	dbCommand      = 2010
	dbCommandReply = 2011
	dbCompressed   = 2012
)

type replyFunc func(err error, rfl *replyFuncLegacyArgs, rfc *replyFuncCommandArgs)

type MongoSocket struct {
	sync.Mutex
	server        *MongoServer // nil when cached
	Conn          net.Conn
	timeout       time.Duration
	addr          string // For debugging only.
	nextRequestId uint32
	replyFuncs    map[uint32]replyFunc
	references    int
	creds         []Credential
	logout        []Credential
	cachedNonce   string
	gotNonce      sync.Cond
	dead          error
	serverInfo    *mongoServerInfo
}

type QueryOpFlags uint32

const (
	_ QueryOpFlags = 1 << iota
	flagTailable
	flagSlaveOk
	flagLogReplay
	flagNoCursorTimeout
	flagAwaitData
)

type QueryOp struct {
	Collection string
	Query      interface{}
	Skip       int32
	Limit      int32
	Selector   interface{}
	Flags      QueryOpFlags
	replyFunc  replyFunc

	mode       Mode
	Options    QueryWrapper
	HasOptions bool
	ServerTags []bson.D
}

func (op *QueryOp) SetReplyFunc(reply replyFunc) {
	op.replyFunc = reply
}

type QueryWrapper struct {
	Query          interface{} "$query"
	OrderBy        interface{} "$orderby,omitempty"
	Hint           interface{} "$hint,omitempty"
	Explain        bool        "$explain,omitempty"
	Snapshot       bool        "$snapshot,omitempty"
	ReadPreference bson.D      "$readPreference,omitempty"
	MaxScan        int         "$maxScan,omitempty"
	MaxTimeMS      int         "$maxTimeMS,omitempty"
	Comment        string      "$comment,omitempty"
}

func (op *QueryOp) finalQuery(socket *MongoSocket) interface{} {
	if op.Flags&flagSlaveOk != 0 && socket.ServerInfo().Mongos {
		var modeName string
		switch op.mode {
		case Strong:
			modeName = "primary"
		case Monotonic, Eventual:
			modeName = "secondaryPreferred"
		case PrimaryPreferred:
			modeName = "primaryPreferred"
		case Secondary:
			modeName = "secondary"
		case SecondaryPreferred:
			modeName = "secondaryPreferred"
		case Nearest:
			modeName = "nearest"
		default:
			panic(fmt.Sprintf("unsupported read mode: %d", op.mode))
		}
		op.HasOptions = true
		op.Options.ReadPreference = make(bson.D, 0, 2)
		op.Options.ReadPreference = append(op.Options.ReadPreference, bson.DocElem{"mode", modeName})
		if len(op.ServerTags) > 0 {
			op.Options.ReadPreference = append(op.Options.ReadPreference, bson.DocElem{"tags", op.ServerTags})
		}
	}
	if op.HasOptions {
		if op.Query == nil {
			var empty bson.D
			op.Options.Query = empty
		} else {
			op.Options.Query = op.Query
		}
		debugf("final query is %#v\n", &op.Options)
		return &op.Options
	}
	return op.Query
}

type GetMoreOp struct {
	Collection string
	Limit      int32
	CursorId   int64
	replyFunc  replyFunc
}

type ReplyOp struct {
	Flags     uint32
	CursorId  int64
	FirstDoc  int32
	ReplyDocs int32
}

type InsertOp struct {
	Collection string        // "database.collection"
	Documents  []interface{} // One or more documents to insert
	Flags      uint32
}

type UpdateOp struct {
	Collection string      `bson:"-"` // "database.collection"
	Selector   interface{} `bson:"q"`
	Update     interface{} `bson:"u"`
	Flags      uint32      `bson:"-"`
	Multi      bool        `bson:"multi,omitempty"`
	Upsert     bool        `bson:"upsert,omitempty"`
}

type DeleteOp struct {
	Collection string      `bson:"-"` // "database.collection"
	Selector   interface{} `bson:"q"`
	Flags      uint32      `bson:"-"`
	Limit      int         `bson:"limit"`
}

type KillCursorsOp struct {
	CursorIds []int64
}

type CommandOp struct {
	Database    string
	CommandName string
	Metadata    interface{}
	CommandArgs interface{}
	InputDocs   []interface{}
	replyFunc   replyFunc
}

type CommandReplyOp struct {
	Metadata     interface{}
	CommandReply interface{}
	OutputDocs   []interface{}
}

// replyFuncCommandArgs contains the arguments needed by the replyFunc to complete a CommandReplyOp.
type replyFuncCommandArgs struct {
	// op is the newly generated CommandReplyOp
	op *CommandReplyOp

	// bytesLeft is the number of bytes that remain to be read by the readLoop.
	// This indicates if there is more data or not so that the reply can decide
	// whether or not to release its lock.
	bytesLeft int

	// metadata is a slice containing the unread bson of the CommandReplyOp's metadata field.
	metadata []byte

	// commandReply is a slice containing the unread bson of the CommandReplyOp's commandReply field.
	commandReply []byte

	// outputDoc is a slice of bytes containing the unread bson of a reply document being handed to the
	// replyFunc
	outputDoc []byte
}

// replyFuncLegacyArgs contains the arguments needed by the replyFunc to complete a ReplyOp.
type replyFuncLegacyArgs struct {
	// op is the newly generated ReplyOp
	op *ReplyOp

	//docNum is the number of the current document being handed to the reply func.
	// This indicates how many docs have been read so the replyFunc can determine if
	// it can release its lock.
	docNum int

	// docData is a slice of bytes containing the unread bson of reply document being handed to the
	// replyFunc
	docData []byte
}

func (op *GetMoreOp) SetReplyFunc(reply replyFunc) {
	op.replyFunc = reply
}
func (op *CommandOp) SetReplyFunc(reply replyFunc) {
	op.replyFunc = reply
}

type OpWithReply interface {
	SetReplyFunc(reply replyFunc)
}

type requestInfo struct {
	bufferPos int
	replyFunc replyFunc
}

func NewSocket(server *MongoServer, conn net.Conn, timeout time.Duration) *MongoSocket {
	socket := &MongoSocket{
		Conn:       conn,
		addr:       server.Addr,
		server:     server,
		replyFuncs: make(map[uint32]replyFunc),
	}
	socket.gotNonce.L = &socket.Mutex
	if err := socket.InitialAcquire(server.Info(), timeout); err != nil {
		panic("newSocket: InitialAcquire returned error: " + err.Error())
	}
	stats.socketsAlive(+1)
	debugf("Socket %p to %s: initialized", socket, socket.addr)
	socket.resetNonce()
	go socket.readLoop()
	return socket
}

func NewDumbSocket(conn net.Conn) *MongoSocket {
	server := &MongoServer{}
	return &MongoSocket{
		server:     server,
		addr:       server.Addr,
		Conn:       conn,
		replyFuncs: make(map[uint32]replyFunc),
	}
}

// Server returns the server that the socket is associated with.
// It returns nil while the socket is cached in its respective server.
func (socket *MongoSocket) Server() *MongoServer {
	socket.Lock()
	server := socket.server
	socket.Unlock()
	return server
}

// ServerInfo returns details for the server at the time the socket
// was initially acquired.
func (socket *MongoSocket) ServerInfo() *mongoServerInfo {
	socket.Lock()
	serverInfo := socket.serverInfo
	socket.Unlock()
	return serverInfo
}

// InitialAcquire obtains the first reference to the socket, either
// right after the connection is made or once a recycled socket is
// being put back in use.
func (socket *MongoSocket) InitialAcquire(serverInfo *mongoServerInfo, timeout time.Duration) error {
	socket.Lock()
	if socket.references > 0 {
		panic("Socket acquired out of cache with references")
	}
	if socket.dead != nil {
		dead := socket.dead
		socket.Unlock()
		return dead
	}
	socket.references++
	socket.serverInfo = serverInfo
	socket.timeout = timeout
	stats.socketsInUse(+1)
	stats.socketRefs(+1)
	socket.Unlock()
	return nil
}

// Acquire obtains an additional reference to the socket.
// The socket will only be recycled when it's released as many
// times as it's been acquired.
func (socket *MongoSocket) Acquire() (info *mongoServerInfo) {
	socket.Lock()
	if socket.references == 0 {
		panic("Socket got non-initial acquire with references == 0")
	}
	// We'll track references to dead sockets as well.
	// Caller is still supposed to release the socket.
	socket.references++
	stats.socketRefs(+1)
	serverInfo := socket.serverInfo
	socket.Unlock()
	return serverInfo
}

// Release decrements a socket reference. The socket will be
// recycled once its released as many times as it's been acquired.
func (socket *MongoSocket) Release() {
	socket.Lock()
	if socket.references == 0 {
		panic("socket.Release() with references == 0")
	}
	socket.references--
	stats.socketRefs(-1)
	if socket.references == 0 {
		stats.socketsInUse(-1)
		server := socket.server
		socket.Unlock()
		socket.LogoutAll()
		// If the socket is dead server is nil.
		if server != nil {
			server.RecycleSocket(socket)
		}
	} else {
		socket.Unlock()
	}
}

// SetTimeout changes the timeout used on socket operations.
func (socket *MongoSocket) SetTimeout(d time.Duration) {
	socket.Lock()
	socket.timeout = d
	socket.Unlock()
}

type deadlineType int

const (
	readDeadline  deadlineType = 1
	writeDeadline deadlineType = 2
)

func (socket *MongoSocket) updateDeadline(which deadlineType) {
	var when time.Time
	if socket.timeout > 0 {
		when = time.Now().Add(socket.timeout)
	}
	whichstr := ""
	switch which {
	case readDeadline | writeDeadline:
		whichstr = "read/write"
		socket.Conn.SetDeadline(when)
	case readDeadline:
		whichstr = "read"
		socket.Conn.SetReadDeadline(when)
	case writeDeadline:
		whichstr = "write"
		socket.Conn.SetWriteDeadline(when)
	default:
		panic("invalid parameter to updateDeadline")
	}
	debugf("Socket %p to %s: updated %s deadline to %s ahead (%s)", socket, socket.addr, whichstr, socket.timeout, when)
}

// Close terminates the socket use.
func (socket *MongoSocket) Close() {
	socket.kill(errors.New("Closed explicitly"), false)
}

func (socket *MongoSocket) kill(err error, abend bool) {
	socket.Lock()
	if socket.dead != nil {
		debugf("Socket %p to %s: killed again: %s (previously: %s)", socket, socket.addr, err.Error(), socket.dead.Error())
		socket.Unlock()
		return
	}
	logf("Socket %p to %s: closing: %s (abend=%v)", socket, socket.addr, err.Error(), abend)
	socket.dead = err
	socket.Conn.Close()
	stats.socketsAlive(-1)
	replyFuncs := socket.replyFuncs
	socket.replyFuncs = make(map[uint32]replyFunc)
	server := socket.server
	socket.server = nil
	socket.gotNonce.Broadcast()
	socket.Unlock()
	for _, replyFunc := range replyFuncs {
		logf("Socket %p to %s: notifying replyFunc of closed socket: %s", socket, socket.addr, err.Error())
		replyFunc(err, nil, nil)
	}
	if abend {
		server.AbendSocket(socket)
	}
}

func (socket *MongoSocket) SimpleQuery(op *QueryOp) (data []byte, replyOp *ReplyOp, err error) {
	var wait, change sync.Mutex
	var replyDone bool
	var replyData []byte
	var replyErr error
	wait.Lock()
	op.replyFunc = func(err error, rfl *replyFuncLegacyArgs, rfc *replyFuncCommandArgs) {
		change.Lock()
		if !replyDone {
			replyDone = true
			replyErr = err
			if rfl != nil {
				replyOp = rfl.op
				if err == nil {
					replyData = rfl.docData
				}
			}
		}
		change.Unlock()
		wait.Unlock()
	}
	err = socket.Query(op)
	if err != nil {
		return nil, nil, err
	}
	wait.Lock()
	change.Lock()
	data = replyData
	err = replyErr
	change.Unlock()
	return data, replyOp, err
}

func (socket *MongoSocket) Query(ops ...interface{}) (err error) {
	if lops := socket.flushLogout(); len(lops) > 0 {
		ops = append(lops, ops...)
	}

	buf := make([]byte, 0, 256)

	// Serialize operations synchronously to avoid interrupting
	// other goroutines while we can't really be sending data.
	// Also, record id positions so that we can compute request
	// ids at once later with the lock already held.
	requests := make([]requestInfo, len(ops))
	requestCount := 0

	for _, op := range ops {
		debugf("Socket %p to %s: serializing op: %#v", socket, socket.addr, op)
		start := len(buf)
		var replyFunc replyFunc
		switch op := op.(type) {

		case *UpdateOp:
			buf = addHeader(buf, dbUpdate)
			buf = addInt32(buf, 0) // Reserved
			buf = addCString(buf, op.Collection)
			buf = addInt32(buf, int32(op.Flags))
			debugf("Socket %p to %s: serializing selector document: %#v", socket, socket.addr, op.Selector)
			buf, err = addBSON(buf, op.Selector)
			if err != nil {
				return err
			}
			debugf("Socket %p to %s: serializing update document: %#v", socket, socket.addr, op.Update)
			buf, err = addBSON(buf, op.Update)
			if err != nil {
				return err
			}

		case *InsertOp:
			buf = addHeader(buf, dbInsert)
			buf = addInt32(buf, int32(op.Flags))
			buf = addCString(buf, op.Collection)
			for _, doc := range op.Documents {
				debugf("Socket %p to %s: serializing document for insertion: %#v", socket, socket.addr, doc)
				buf, err = addBSON(buf, doc)
				if err != nil {
					return err
				}
			}

		case *QueryOp:
			buf = addHeader(buf, dbQuery)
			buf = addInt32(buf, int32(op.Flags))
			buf = addCString(buf, op.Collection)
			buf = addInt32(buf, op.Skip)
			buf = addInt32(buf, op.Limit)
			buf, err = addBSON(buf, op.finalQuery(socket))
			if err != nil {
				return err
			}
			if op.Selector != nil {
				buf, err = addBSON(buf, op.Selector)
				if err != nil {
					return err
				}
			}
			replyFunc = op.replyFunc

		case *GetMoreOp:
			buf = addHeader(buf, dbGetMore)
			buf = addInt32(buf, 0) // Reserved
			buf = addCString(buf, op.Collection)
			buf = addInt32(buf, op.Limit)
			buf = addInt64(buf, op.CursorId)
			replyFunc = op.replyFunc

		case *ReplyOp:
			buf = addHeader(buf, opReply)
			buf = addInt32(buf, int32(op.Flags))
			buf = addInt64(buf, op.CursorId)
			buf = addInt32(buf, op.FirstDoc)
			buf = addInt32(buf, op.ReplyDocs)

		case *DeleteOp:
			buf = addHeader(buf, dbDelete)
			buf = addInt32(buf, 0) // Reserved
			buf = addCString(buf, op.Collection)
			buf = addInt32(buf, int32(op.Flags))
			debugf("Socket %p to %s: serializing selector document: %#v", socket, socket.addr, op.Selector)
			buf, err = addBSON(buf, op.Selector)
			if err != nil {
				return err
			}

		case *KillCursorsOp:
			buf = addHeader(buf, dbKillCursors)
			buf = addInt32(buf, 0) // Reserved
			buf = addInt32(buf, int32(len(op.CursorIds)))
			for _, cursorId := range op.CursorIds {
				buf = addInt64(buf, cursorId)
			}
		case *CommandOp:
			buf = addHeader(buf, dbCommand)
			buf = addCString(buf, op.Database)
			buf = addCString(buf, op.CommandName)
			buf, err = addBSON(buf, op.CommandArgs)
			if err != nil {
				return err
			}
			buf, err = addBSON(buf, op.Metadata)
			if err != nil {
				return err
			}
			for _, doc := range op.InputDocs {
				debugf("Socket %p to %s: serializing document for opcommand: %#v", socket, socket.addr, doc)
				buf, err = addBSON(buf, doc)
				if err != nil {
					return err
				}
			}
			replyFunc = op.replyFunc
		case *CommandReplyOp:
			buf = addHeader(buf, dbCommandReply)
			buf, err = addBSON(buf, op.CommandReply)
			if err != nil {
				return err
			}
			buf, err = addBSON(buf, op.Metadata)
			if err != nil {
				return err
			}
			for _, doc := range op.OutputDocs {
				debugf("Socket %p to %s: serializing document for opcommand: %#v", socket, socket.addr, doc)
				buf, err = addBSON(buf, doc)
				if err != nil {
					return err
				}
			}

		default:
			panic("internal error: unknown operation type")
		}

		setInt32(buf, start, int32(len(buf)-start))

		if replyFunc != nil {
			request := &requests[requestCount]
			request.replyFunc = replyFunc
			request.bufferPos = start
			requestCount++
		}
	}

	// Buffer is ready for the pipe.  Lock, allocate ids, and enqueue.

	socket.Lock()
	if socket.dead != nil {
		dead := socket.dead
		socket.Unlock()
		debugf("Socket %p to %s: failing query, already closed: %s", socket, socket.addr, socket.dead.Error())
		// XXX This seems necessary in case the session is closed concurrently
		// with a query being performed, but it's not yet tested:
		for i := 0; i != requestCount; i++ {
			request := &requests[i]
			if request.replyFunc != nil {
				request.replyFunc(dead, nil, nil)
			}
		}
		return dead
	}

	wasWaiting := len(socket.replyFuncs) > 0

	// Reserve id 0 for requests which should have no responses.
	requestId := socket.nextRequestId + 1
	if requestId == 0 {
		requestId++
	}
	socket.nextRequestId = requestId + uint32(requestCount)
	for i := 0; i != requestCount; i++ {
		request := &requests[i]
		setInt32(buf, request.bufferPos+4, int32(requestId))
		socket.replyFuncs[requestId] = request.replyFunc
		requestId++
	}

	debugf("Socket %p to %s: sending %d op(s) (%d bytes)", socket, socket.addr, len(ops), len(buf))
	stats.sentOps(len(ops))

	socket.updateDeadline(writeDeadline)
	_, err = socket.Conn.Write(buf)

	if !wasWaiting && requestCount > 0 {
		socket.updateDeadline(readDeadline)
	}
	socket.Unlock()
	return err
}

// Estimated minimum cost per socket: 1 goroutine + memory for the largest
// document ever seen.
func (socket *MongoSocket) readLoop() {
	headerBuf := make([]byte, 16)        // 16 from header
	opReplyFieldsBuf := make([]byte, 20) // 20 from OP_REPLY fixed fields
	for {
		var r io.Reader = socket.Conn

		// XXX Handle timeouts, , etc
		_, err := io.ReadFull(r, headerBuf)
		if err != nil {
			socket.kill(err, true)
			return
		}

		totalLen := getInt32(headerBuf, 0)
		responseTo := getInt32(headerBuf, 8)
		opCode := getInt32(headerBuf, 12)

		if opCode == dbCompressed {
			buf := bytes.NewBuffer(headerBuf)
			io.CopyN(buf, r, int64(totalLen-16))
			msg, err := DecompressMessage(buf.Bytes())
			if err != nil {
				socket.kill(err, true)
				return
			}
			r = bytes.NewBuffer(msg)

			_, err = io.ReadFull(r, headerBuf)
			if err != nil {
				socket.kill(err, true)
				return
			}
			opCode = getInt32(headerBuf, 12)
			if opCode == dbCompressed {
				err = fmt.Errorf("cannot recursively decompress messages")
				socket.kill(err, true)
				return
			}
		}

		// Don't use socket.server.Addr here.  socket is not
		// locked and socket.server may go away.
		debugf("Socket %p to %s: got reply (%d bytes)", socket, socket.addr, totalLen)

		socket.Lock()
		replyFunc, ok := socket.replyFuncs[uint32(responseTo)]
		if ok {
			delete(socket.replyFuncs, uint32(responseTo))
		}
		socket.Unlock()

		switch opCode {
		case opReply:
			_, err := io.ReadFull(r, opReplyFieldsBuf)
			if err != nil {
				socket.kill(err, true)
				return
			}
			reply := ReplyOp{
				Flags:     uint32(getInt32(opReplyFieldsBuf, 0)),
				CursorId:  getInt64(opReplyFieldsBuf, 4),
				FirstDoc:  getInt32(opReplyFieldsBuf, 12),
				ReplyDocs: getInt32(opReplyFieldsBuf, 16),
			}
			stats.receivedOps(+1)
			stats.receivedDocs(int(reply.ReplyDocs))
			if replyFunc != nil && reply.ReplyDocs == 0 {
				rfl := replyFuncLegacyArgs{
					op:     &reply,
					docNum: -1,
				}
				replyFunc(nil, &rfl, nil)
			} else {
				for i := 0; i != int(reply.ReplyDocs); i++ {
					b, err := readDocument(r)
					if err != nil {
						if replyFunc != nil {
							replyFunc(err, &replyFuncLegacyArgs{docNum: -1}, nil)
						}
						socket.kill(err, true)
						return
					}

					if replyFunc != nil {
						rfl := replyFuncLegacyArgs{
							op:      &reply,
							docNum:  i,
							docData: b,
						}
						replyFunc(nil, &rfl, nil)
					}
					// XXX Do bound checking against totalLen.
				}
			}
		case dbCommandReply:
			commandReplyAsSlice, err := readDocument(r)
			if err != nil {
				socket.kill(err, true)
				return
			}
			metadataAsSlice, err := readDocument(r)
			if err != nil {
				socket.kill(err, true)
				return
			}
			rfc := replyFuncCommandArgs{
				op:           &CommandReplyOp{},
				metadata:     metadataAsSlice,
				commandReply: commandReplyAsSlice,
			}
			lengthRead := len(commandReplyAsSlice) + len(metadataAsSlice)
			if replyFunc != nil && lengthRead+16 >= int(totalLen) {
				replyFunc(nil, nil, &rfc)
			}

			docLen := 0
			for lengthRead+docLen < int(totalLen)-16 {
				documentBuf, err := readDocument(r)
				if err != nil {
					rfc.bytesLeft = 0
					if replyFunc != nil {
						replyFunc(err, nil, &rfc)
					}
					socket.kill(err, true)
					return
				}
				rfc.outputDoc = documentBuf
				if replyFunc != nil {
					replyFunc(nil, nil, &rfc)
				}
				docLen += len(documentBuf)
			}
		default:
			socket.kill(errors.New("opcode != 1 or 2011, corrupted data?"), true)
			return
		}

		socket.Lock()
		if len(socket.replyFuncs) == 0 {
			// Nothing else to read for now. Disable deadline.
			socket.Conn.SetReadDeadline(time.Time{})
		} else {
			socket.updateDeadline(readDeadline)
		}
		socket.Unlock()

		// XXX Do bound checking against totalLen.
	}
}

func readDocument(r io.Reader) (docBuf []byte, err error) {
	sizeBuf := make([]byte, 4)
	_, err = io.ReadFull(r, sizeBuf)
	if err != nil {
		return
	}
	size := getInt32(sizeBuf, 0)
	docBuf = make([]byte, int(size))

	copy(docBuf, sizeBuf)

	_, err = io.ReadFull(r, docBuf[4:])
	if err != nil {
		return
	}
	if globalDebug && globalLogger != nil {
		m := bson.M{}
		if err := bson.Unmarshal(docBuf, m); err == nil {
			if conn, ok := r.(net.Conn); ok {
				debugf("Socket with addr '%s' received document: %#v", conn.RemoteAddr(), m)
			}
		}
	}
	return
}

var emptyHeader = []byte{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}

func addHeader(b []byte, opcode int) []byte {
	i := len(b)
	b = append(b, emptyHeader...)
	// Enough for current opcodes.
	b[i+12] = byte(opcode)
	b[i+13] = byte(opcode >> 8)
	return b
}

func addInt32(b []byte, i int32) []byte {
	return append(b, byte(i), byte(i>>8), byte(i>>16), byte(i>>24))
}

func addInt64(b []byte, i int64) []byte {
	return append(b, byte(i), byte(i>>8), byte(i>>16), byte(i>>24),
		byte(i>>32), byte(i>>40), byte(i>>48), byte(i>>56))
}

func addCString(b []byte, s string) []byte {
	b = append(b, []byte(s)...)
	b = append(b, 0)
	return b
}

func addBSON(b []byte, doc interface{}) ([]byte, error) {
	if doc == nil {
		return append(b, 5, 0, 0, 0, 0), nil
	}
	data, err := bson.Marshal(doc)
	if err != nil {
		return b, err
	}
	return append(b, data...), nil
}

func setInt32(b []byte, pos int, i int32) {
	b[pos] = byte(i)
	b[pos+1] = byte(i >> 8)
	b[pos+2] = byte(i >> 16)
	b[pos+3] = byte(i >> 24)
}

func getInt32(b []byte, pos int) int32 {
	return (int32(b[pos+0])) |
		(int32(b[pos+1]) << 8) |
		(int32(b[pos+2]) << 16) |
		(int32(b[pos+3]) << 24)
}

func getInt64(b []byte, pos int) int64 {
	return (int64(b[pos+0])) |
		(int64(b[pos+1]) << 8) |
		(int64(b[pos+2]) << 16) |
		(int64(b[pos+3]) << 24) |
		(int64(b[pos+4]) << 32) |
		(int64(b[pos+5]) << 40) |
		(int64(b[pos+6]) << 48) |
		(int64(b[pos+7]) << 56)
}
