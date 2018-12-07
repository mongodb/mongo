package mongoreplay

import (
	"bytes"
	"io"
	"testing"
	"time"

	"github.com/10gen/llmgo/bson"
)

func TestRepeatGeneration(t *testing.T) {
	recOp := &RecordedOp{
		Seen: &PreciseTime{time.Now()},
	}
	bsonBytes, err := bson.Marshal(recOp)
	if err != nil {
		t.Errorf("couldn't marshal %v", err)
	}
	playbackReader := &PlaybackFileReader{bytes.NewReader(bsonBytes)}

	repeat := 2
	opChan, errChan := NewOpChanFromFile(playbackReader, repeat)
	op1, ok := <-opChan
	if !ok {
		t.Errorf("read of 0-generation op failed")
	}
	if op1.Generation != 0 {
		t.Errorf("generation of 0 generation op is %v", op1.Generation)
	}
	op2, ok := <-opChan
	if !ok {
		t.Errorf("read of 1-generation op failed")
	}
	if op2.Generation != 1 {
		t.Errorf("generation of 1 generation op is %v", op2.Generation)
	}
	_, ok = <-opChan
	if ok {
		t.Errorf("Successfully read past end of op chan")
	}
	err = <-errChan
	if err != io.EOF {
		t.Errorf("should have eof at end, but got %v", err)
	}
}

func TestPlayOpEOF(t *testing.T) {
	ops := []RecordedOp{{
		Seen: &PreciseTime{time.Now()},
	}, {
		Seen: &PreciseTime{time.Now()},
		EOF:  true,
	}}
	var buf bytes.Buffer
	for _, op := range ops {
		bsonBytes, err := bson.Marshal(op)
		if err != nil {
			t.Errorf("couldn't marshal op %v", err)
		}
		buf.Write(bsonBytes)
	}
	playbackReader := &PlaybackFileReader{bytes.NewReader(buf.Bytes())}

	repeat := 2
	opChan, errChan := NewOpChanFromFile(playbackReader, repeat)

	op1, ok := <-opChan
	if !ok {
		t.Errorf("read of op1 failed")
	}
	if op1.EOF {
		t.Errorf("op1 should not be an EOF op")
	}
	op2, ok := <-opChan
	if !ok {
		t.Errorf("read op2 failed")
	}
	if op2.EOF {
		t.Errorf("op2 should not be an EOF op")
	}
	op3, ok := <-opChan
	if !ok {
		t.Errorf("read of op3 failed")
	}
	if !op3.EOF {
		t.Errorf("op3 is not an EOF op")
	}

	_, ok = <-opChan
	if ok {
		t.Errorf("Successfully read past end of op chan")
	}
	err := <-errChan
	if err != io.EOF {
		t.Errorf("should have eof at end, but got %v", err)
	}
}
