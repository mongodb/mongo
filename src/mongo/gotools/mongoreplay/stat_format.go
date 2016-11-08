package mongoreplay

import (
	"bytes"
	"encoding/json"
	"fmt"
	"time"

	"github.com/10gen/escaper"
)

// OpStat is a set of metadata about an executed operation and its result which can be
// used for generating reports about the results of a playback command.
type OpStat struct {
	// Order is a number denoting the position in the traffic in which this operation appeared
	Order int64 `json:"order"`

	// OpType is a string representation of the function of this operation. For example an 'insert'
	// or a 'query'
	OpType string `json:"op,omitempty"`

	// If the operation was a command, this field represents the name of the database command
	// performed, like "isMaster" or "getLastError". Left blank for ops that are not commands
	// like a query, insert, or getmore.
	Command string `json:"command,omitempty"`

	// Namespace that the operation was performed against, if relevant.
	Ns string `json:"ns,omitempty"`

	// Data represents the payload of the request operation.
	RequestData interface{} `json:"request_data, omitempty"`

	// Data represents the payload of the reply operation.
	ReplyData interface{} `json:"reply_data, omitempty"`

	// NumReturned is the number of documents that were fetched as a result of this operation.
	NumReturned int `json:"nreturned,omitempty"`

	// PlayedAt is the time that this operation was replayed
	PlayedAt *time.Time `json:"played_at,omitempty"`

	// PlayAt is the time that this operation is scheduled to be played. It represents the time
	// that it is supposed to be played by mongoreplay, but can be different from
	// PlayedAt if the playback is lagging for any reason
	PlayAt *time.Time `json:"play_at,omitempty"`

	// PlaybackLagMicros is the time difference in microseconds between the time
	// that the operation was supposed to be played, and the time it was actualy played.
	// High values indicate that playback is falling behind the intended rate.
	PlaybackLagMicros int64 `json:"playbacklag_us,omitempty"`

	// ConnectionNum represents the number of the connection that the op originated from.
	// This number does not correspond to any server-side connection IDs - it's simply an
	// auto-incrementing number representing the order in which the connection was created
	// during the playback phase.
	ConnectionNum int64 `json:"connection_num"`

	// LatencyMicros represents the time difference in microseconds between when the operation
	// was executed and when the reply from the server was received.
	LatencyMicros int64 `json:"latency_us,omitempty"`

	// Errors contains the error messages returned from the server populated in the $err field.
	// If unset, the operation did not receive any errors from the server.
	Errors []error `json:"errors,omitempty"`

	Message string `json:"msg,omitempty"`

	// Seen is the time that this operation was originally seen.
	Seen *time.Time `json:"seen,omitempty"`

	// RequestID is the ID of the mongodb operation as taken from the header.
	// The RequestID for a request operation is the same as the ResponseID for
	// the corresponding reply, so this field will be the same for request/reply pairs.
	RequestID int32 `json:"request_id, omitempty"`
}

// jsonGet retrieves serialized json req/res via the channel-like arg;
// allows expanded output string to be blocking only when necessary.
func jsonGet(wBuf *bufferWaiter) func(string) string {
	return func(arg string) string {
		buf := wBuf.Get()
		if buf == nil {
			return ""
		}
		if arg == "" {
			return buf.String()
		}
		// retrieve field inside json
		m := make(map[string]interface{})
		err := json.Unmarshal(buf.Bytes(), &m)
		if err != nil {
			return err.Error()
		}

		data, ok := getDotField(m, arg)
		if !ok {
			return "N/A"
		}
		switch v := data.(type) {
		case map[string]interface{}:
		case []interface{}:
		default:
			return fmt.Sprintf("%v", v)
		}

		// data is a slice or map, re-serialize
		b, err := json.Marshal(data)
		if err != nil {
			return err.Error()
		}
		return fmt.Sprintf("%s", b)
	}
}

// cond returns a function which is identity if data is not nil,
// and which otherwise always returns the empty string.
func cond(data interface{}) func(string) string {
	return func(arg string) (out string) {
		if data != nil {
			out = arg
		}
		return
	}
}

func (stat *OpStat) escaper(req, res <-chan *bytes.Buffer) *escaper.Escaper {
	esc := escaper.Default()

	wReq, wRes := newBufferWaiter(req), newBufferWaiter(res)
	esc.Register('n', stat.getNs)
	esc.Register('l', stat.getLatency)
	esc.Register('T', stat.getOpType)
	esc.Register('c', stat.getCommand)
	esc.Register('o', stat.getConnectionNum)
	esc.Register('i', stat.getRequestID)
	esc.RegisterArg('t', stat.getTime)
	esc.RegisterArg('q', jsonGet(wReq))
	esc.RegisterArg('r', jsonGet(wRes))
	esc.RegisterArg('Q', cond(stat.RequestData))
	esc.RegisterArg('R', cond(stat.ReplyData))
	return esc
}

func (stat *OpStat) getOpType() string {
	return stat.OpType
}
func (stat *OpStat) getCommand() string {
	return stat.Command
}
func (stat *OpStat) getNs() string {
	return stat.Ns
}
func (stat *OpStat) getConnectionNum() string {
	return fmt.Sprintf("%d", stat.ConnectionNum)
}
func (stat *OpStat) getRequestID() string {
	return fmt.Sprintf("%d", stat.RequestID)
}
func (stat *OpStat) getTime(layout string) string {
	if layout == "" {
		layout = time.RFC822Z
	}
	t := stat.Seen
	if stat.PlayedAt != nil {
		t = stat.PlayedAt
	}
	return t.Format(layout)
}
func (stat *OpStat) getLatency() string {
	if stat.LatencyMicros <= 0 {
		return "" // N/A
	}
	latency := time.Microsecond * time.Duration(stat.LatencyMicros)
	return fmt.Sprintf("+%s", latency)
}
