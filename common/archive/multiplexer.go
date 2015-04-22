package archive

import (
	"fmt"
	"github.com/mongodb/mongo-tools/common/intents"
	"gopkg.in/mgo.v2/bson"
	"io"
	"os"
	"reflect"
	"sync"
)

type Multiplexer struct {
	Out                  io.Writer
	Control              chan byte
	selectCasesLock      sync.Mutex
	selectCasesNamespace []string
	ins                  []*MuxIn
	selectCases          []reflect.SelectCase
	currentNamespace     string
}

// Run multiplexes until it receives an EOF on its Control chan.
func (mux *Multiplexer) Run() error {
	var err error
	defer func() {
		// XXX reimplement this
		fmt.Fprintf(os.Stderr, "mux err: %v", err)
	}()
	for {
		selectCases, ins := mux.getSelectCases()
		selectCases = append(selectCases, reflect.SelectCase{
			Dir:  reflect.SelectRecv,
			Chan: reflect.ValueOf(mux.Control),
			Send: reflect.Value{},
		})
		controlIndex := len(selectCases) - 1
		index, value, notEOF := reflect.Select(selectCases)
		if index == controlIndex {
			if notEOF {
				continue
			}
			return nil
		}
		bsonBytes, ok := value.Interface().([]byte)
		if !ok {
			return fmt.Errorf("multiplexer received a value that wasn't a []byte")
		}
		if notEOF {
			err = mux.formatBody(ins[index], bsonBytes)
			if err != nil {
				return err
			}
		} else {
			err = mux.formatEOF(index, ins[index])
			if err != nil {
				return err
			}
		}
	}
}

// formatBody writes the BSON in to the archive, potentially writing a new header
// if the document belongs to a different namespace from the last header.
func (mux *Multiplexer) formatBody(in *MuxIn, bsonBytes []byte) error {
	var err error
	if in.Intent.Namespace() != mux.currentNamespace {
		// Handle the change of which DB/Collection we're writing docs for
		if mux.currentNamespace != "" {
			_, err = mux.Out.Write(terminatorBytes)
			if err != nil {
				return err
			}
		}
		header, err := bson.Marshal(NamespaceHeader{
			Database:   in.Intent.DB,
			Collection: in.Intent.C,
		})
		if err != nil {
			return err
		}
		_, err = mux.Out.Write(header)
		if err != nil {
			return err
		}
	}
	mux.currentNamespace = in.Intent.Namespace()
	length, err := mux.Out.Write(bsonBytes)
	if err != nil {
		return err
	}
	in.writeLenChan <- length
	return nil
}

// formatEOF writes the EOF header in to the archive
func (mux *Multiplexer) formatEOF(index int, in *MuxIn) error {
	var err error
	if mux.currentNamespace != "" {
		_, err = mux.Out.Write(terminatorBytes)
		if err != nil {
			return err
		}
	}
	eofHeader, err := bson.Marshal(NamespaceHeader{Database: in.Intent.DB, Collection: in.Intent.C, EOF: true})
	if err != nil {
		return err
	}
	_, err = mux.Out.Write(eofHeader)
	if err != nil {
		return err
	}
	_, err = mux.Out.Write(terminatorBytes)
	if err != nil {
		return err
	}
	mux.currentNamespace = ""
	mux.close(index)
	return nil
}

// getSelectCasesAndIns simply returns the SelectCases and MuxIns from the multiplexer
// the lock is locked because the select cases and ins are update by the MuxIn's.
func (mux *Multiplexer) getSelectCases() ([]reflect.SelectCase, []*MuxIn) {
	mux.selectCasesLock.Lock()
	cases, ins := mux.selectCases, mux.ins
	mux.selectCasesLock.Unlock()
	return cases, ins
}

// close creates new slices of SelectCases and MuxIns, sans the one specified
// by index and replaces the selectCases and MuxIns in the Multiplexer.
// It does not close the MuxIn, because that should have already been done and
// should have been what has caused this close to occur.
func (mux *Multiplexer) close(index int) {
	mux.selectCasesLock.Lock()
	defer mux.selectCasesLock.Unlock()
	// create brand new slices to avoid clobbering any acquired via getSelectCases()
	ins := make([]*MuxIn, 0, len(mux.ins)-1)
	ins = append(ins, mux.ins[:index]...)
	ins = append(ins, mux.ins[index+1:]...)
	mux.ins = ins

	selectCases := make([]reflect.SelectCase, 0, len(mux.selectCases)) // extra space for the control chan
	selectCases = append(selectCases, mux.selectCases[:index]...)
	selectCases = append(selectCases, mux.selectCases[index+1:]...)
	mux.selectCases = selectCases
}

// open creates new slices of SelectCases and MuxIns, adding the passed MuxIn
// as well as a new corresponding SelectCase, containing newly created chans.
func (mux *Multiplexer) open(mxIn *MuxIn) {
	mux.selectCasesLock.Lock()
	defer mux.selectCasesLock.Unlock()
	writeChan := make(chan []byte)
	mxIn.writeChan = writeChan
	mxIn.writeLenChan = make(chan int)
	// create brand new slices to avoid clobbering any acquired via getSelectCases()
	ins := make([]*MuxIn, 0, len(mux.ins)+1)
	ins = append(ins, mux.ins...)
	mux.ins = append(ins, mxIn)

	selectCases := make([]reflect.SelectCase, 0, len(mux.selectCases)+2) // extra space for the control chan
	selectCases = append(selectCases, mux.selectCases...)
	mux.selectCases = append(selectCases,
		reflect.SelectCase{
			Dir:  reflect.SelectRecv,
			Chan: reflect.ValueOf(writeChan),
			Send: reflect.Value{},
		})
}

// MuxIn is an implementation of the intent.file interface.
// They live in the intents, and are potentially owned by different threads than
// the thread owning the Multiplexer.
// They are out the intents write data to the multiplexer
type MuxIn struct {
	writeChan    chan<- []byte
	writeLenChan chan int
	Intent       *intents.Intent
	Mux          *Multiplexer
}

// Read does nothing for MuxIns
func (mxIn *MuxIn) Read([]byte) (int, error) {
	return 0, nil
}

// Close closes the chans in the MuxIn.
// Ultimately the multiplexer will detect that they are closed and cause a
// formatEOF to occur.
func (mxIn *MuxIn) Close() error {
	// the mux side of this gets closed in the mux when it gets an eof on the read
	close(mxIn.writeChan)
	close(mxIn.writeLenChan)
	return nil
}

// Open is implemented in Mux.open, but in short, it creates chans and a select case
// and adds the SelectCase and the MuxIn in to the Multiplexer.
func (mxIn *MuxIn) Open() error {
	mxIn.Mux.open(mxIn)
	mxIn.Mux.Control <- 0
	return nil
}

// Write hands a buffer to the Multiplexer and receives a written length from the multiplexer
// after the length is received, the buffer is free to be reused.
func (mxIn *MuxIn) Write(buf []byte) (int, error) {
	mxIn.writeChan <- buf
	length := <-mxIn.writeLenChan
	return length, nil
}
