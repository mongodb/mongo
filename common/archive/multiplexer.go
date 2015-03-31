package archive

import (
	"fmt"
	"gopkg.in/mgo.v2/bson"
	"io"
	"reflect"
	"strings"
	"sync"
)

type Multiplexer struct {
	out                  io.Writer
	selectCasesLock      sync.Mutex
	selectCasesNamespace []string
	ins                  []*MuxIn
	selectCases          []reflect.SelectCase
	currentNamespace     string
}

// Run multiplexes the inputs while inputs are open
func (mux *Multiplexer) Run() error {
	var err error
	for {
		selectCases, ins := mux.getSelectCases()
		if len(selectCases) == 0 {
			// you must start writers before you start the mux, otherwise the mux will just finish thinking there is no more work to do
			return nil
		}
		index, value, selectOk := reflect.Select(selectCases)
		bsonBytes, ok := value.Interface().([]byte)
		if !ok {
			return fmt.Errorf("Multiplexer received a value that wasn't a []byte")
		}
		if !selectOk {
			err = mux.formatBody(ins[index], bsonBytes)
			if err != nil {
				return err
			}
		} else {
			err = mux.formatEOF(index, ins[index].namespace)
			if err != nil {
				return err
			}
		}
	}
}

// formatBody writes the bson in to the archive, potentially writing a new header
// if the doc belongs to a different namespace from the last header
func (mux *Multiplexer) formatBody(in *MuxIn, bsonBytes []byte) error {
	var err error
	if in.namespace != mux.currentNamespace {
		// Handle the change of which DB/Collection we're writing docs for
		if mux.currentNamespace != "" {
			_, err = mux.out.Write(terminatorBytes)
			if err != nil {
				return err
			}
		}
		namespaceParts := strings.SplitN(in.namespace, ".", 2)
		header, err := bson.Marshal(CollectionHeader{Database: namespaceParts[0], Collection: namespaceParts[1]})
		if err != nil {
			return err
		}
		_, err = mux.out.Write(header)
		if err != nil {
			return err
		}
	}
	mux.currentNamespace = in.namespace
	length, err := mux.out.Write(bsonBytes)
	if err != nil {
		return err
	}
	in.writeLenChan <- length
	return nil
}

// foratEOF writes the EOF header in to the archive
func (mux *Multiplexer) formatEOF(index int, namespace string) error {
	var err error
	if mux.currentNamespace != "" {
		_, err = mux.out.Write(terminatorBytes)
		if err != nil {
			return err
		}
	}
	namespaceParts := strings.SplitN(namespace, ".", 2)
	eofHeader, err := bson.Marshal(CollectionHeader{Database: namespaceParts[0], Collection: namespaceParts[1], EOF: true})
	if err != nil {
		return err
	}
	_, err = mux.out.Write(eofHeader)
	if err != nil {
		return err
	}
	_, err = mux.out.Write(terminatorBytes)
	if err != nil {
		return err
	}
	mux.currentNamespace = ""
	mux.close(index)
	return nil
}

// getSelectCasesAndIns simply returns the SelectCases and MuxIns from the multiplexer
// the lock is locked because the select cases and ins are update by the MuxIn's
func (mux *Multiplexer) getSelectCases() ([]reflect.SelectCase, []*MuxIn) {
	mux.selectCasesLock.Lock()
	cases, ins := mux.selectCases, mux.ins
	mux.selectCasesLock.Unlock()
	return cases, ins
}

// close creates new slices of SelectCases and MuxIns, sans the one specified
// by index and replaces the selectCases and MuxIns in the Multiplexer
// it does not close the MuxIn, because that should have already been done and
// should have been what has caused this close to occur
func (mux *Multiplexer) close(index int) {
	mux.selectCasesLock.Lock()
	defer mux.selectCasesLock.Unlock()
	// create brand new slices to avoid clobbering any acquired via getSelectCases()
	ins := make([]*MuxIn, 0, len(mux.ins)-1)
	ins = append(ins, mux.ins[:index]...)
	ins = append(ins, mux.ins[index+1:]...)
	mux.ins = ins

	selectCases := make([]reflect.SelectCase, 0, len(mux.selectCases)-1)
	selectCases = append(selectCases, mux.selectCases[:index]...)
	selectCases = append(selectCases, mux.selectCases[index+1:]...)
	mux.selectCases = selectCases
}

// open creates new slices of SelectCases and MuxIns, adding the passed MuxIn
// as well as a new corresponding SelectCase, containing newly created chans
func (mux *Multiplexer) open(min *MuxIn) {
	mux.selectCasesLock.Lock()
	defer mux.selectCasesLock.Unlock()
	writeChan := make(chan []byte)
	min.writeChan = writeChan
	min.writeLenChan = make(chan int)
	// create brand new slices to avoid clobbering any acquired via getSelectCases()
	ins := make([]*MuxIn, 0, len(mux.ins)+1)
	ins = append(ins, mux.ins...)
	mux.ins = append(ins, min)

	selectCases := make([]reflect.SelectCase, 0, len(mux.selectCases)+1)
	selectCases = append(selectCases, mux.selectCases...)
	mux.selectCases = append(selectCases,
		reflect.SelectCase{
			Dir:  reflect.SelectRecv,
			Chan: reflect.ValueOf(writeChan),
			Send: reflect.Value{},
		})
}

// MuxInx is an implementation of the intent.file interface
// They live in the intents, and are potentially owned by different threads than
// the thread owning the Multiplexer
// They are out the intents write data to the multiplexer
type MuxIn struct {
	writeChan    chan<- []byte
	writeLenChan chan int
	namespace    string
	mux          *Multiplexer
}

// Read does nothing for MuxIns
func (mxIn *MuxIn) Read([]byte) (int, error) {
	return 0, nil
}

// Close closes the chans in the MuxIn
// Ultimately the multiplexer will detect that they are closed and cause a
// formatEOF to occur
func (mxIn *MuxIn) Close() error {
	// the mux side of this gets closed in the mux when it gets an eof on the read
	close(mxIn.writeChan)
	close(mxIn.writeLenChan)
	return nil
}

// Open is implemented in Mux.open, but in short, it creates chans and a select case
// and adds the SelectCase and the MuxIn in to the Multiplexer
func (mxIn *MuxIn) Open() error {
	mxIn.mux.open(mxIn)
	return nil
}

// Write hands a buffer to the Multiplexer and receives a written length from the multiplexer
// after the length is received, the buffer is free to be reused
func (mxIn *MuxIn) Write(buf []byte) (int, error) {
	mxIn.writeChan <- buf
	length := <-mxIn.writeLenChan
	return length, nil
}
