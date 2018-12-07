package archive

import (
	"bytes"
	"fmt"
	"hash"
	"hash/crc64"
	"io"
	"sync"
	"sync/atomic"

	"github.com/mongodb/mongo-tools/common/db"
	"github.com/mongodb/mongo-tools/common/intents"
	"github.com/mongodb/mongo-tools/common/log"
	"gopkg.in/mgo.v2/bson"
)

// DemuxOut is a Demultiplexer output consumer
// The Write() and Close() occur in the same thread as the Demultiplexer runs in.
type DemuxOut interface {
	Write([]byte) (int, error)
	Close() error
	Sum64() (uint64, bool)
}

const (
	NamespaceUnopened = iota
	NamespaceOpened
	NamespaceClosed
)

// Demultiplexer implements Parser.
type Demultiplexer struct {
	In io.Reader
	//TODO wrap up these three into a structure
	outs               map[string]DemuxOut
	lengths            map[string]int64
	currentNamespace   string
	buf                [db.MaxBSONSize]byte
	NamespaceChan      chan string
	NamespaceErrorChan chan error
	NamespaceStatus    map[string]int
}

func CreateDemux(namespaceMetadatas []*CollectionMetadata, in io.Reader) *Demultiplexer {
	demux := &Demultiplexer{
		NamespaceStatus: make(map[string]int),
		In:              in,
	}
	for _, cm := range namespaceMetadatas {
		ns := cm.Database + "." + cm.Collection
		demux.NamespaceStatus[ns] = NamespaceUnopened

	}
	return demux
}

// Run creates and runs a parser with the Demultiplexer as a consumer
func (demux *Demultiplexer) Run() error {
	parser := Parser{In: demux.In}
	err := parser.ReadAllBlocks(demux)
	if len(demux.outs) > 0 {
		log.Logvf(log.Always, "demux finishing when there are still outs (%v)", len(demux.outs))
	}

	log.Logvf(log.DebugLow, "demux finishing (err:%v)", err)
	return err
}

type demuxError struct {
	Err error
	Msg string
}

// Error is part of the Error interface. It formats a demuxError for human readability.
func (pe *demuxError) Error() string {
	err := fmt.Sprintf("error demultiplexing archive; %v", pe.Msg)
	if pe.Err != nil {
		err = fmt.Sprintf("%v ( %v )", err, pe.Err)
	}
	return err
}

// newError creates a demuxError with just a message
func newError(msg string) error {
	return &demuxError{
		Msg: msg,
	}
}

// newWrappedError creates a demuxError with a message as well as an underlying cause error
func newWrappedError(msg string, err error) error {
	return &demuxError{
		Err: err,
		Msg: msg,
	}
}

// HeaderBSON is part of the ParserConsumer interface and receives headers from parser.
// Its main role is to implement opens and EOFs of the embedded stream.
func (demux *Demultiplexer) HeaderBSON(buf []byte) error {
	colHeader := NamespaceHeader{}
	err := bson.Unmarshal(buf, &colHeader)
	if err != nil {
		return newWrappedError("header bson doesn't unmarshal as a collection header", err)
	}
	log.Logvf(log.DebugHigh, "demux namespaceHeader: %v", colHeader)
	if colHeader.Collection == "" {
		return newError("collection header is missing a Collection")
	}
	demux.currentNamespace = colHeader.Database + "." + colHeader.Collection
	if _, ok := demux.outs[demux.currentNamespace]; !ok {
		if demux.NamespaceStatus[demux.currentNamespace] != NamespaceUnopened {
			return newError("namespace header for already opened namespace")
		}
		demux.NamespaceStatus[demux.currentNamespace] = NamespaceOpened
		if demux.NamespaceChan != nil {
			demux.NamespaceChan <- demux.currentNamespace
			err := <-demux.NamespaceErrorChan
			if err == io.EOF {
				// if the Prioritizer sends us back an io.EOF then it's telling us that
				// it's finishing and doesn't need any more namespace announcements.
				close(demux.NamespaceChan)
				demux.NamespaceChan = nil
				return nil
			}
			if err != nil {
				return newWrappedError("failed arranging a consumer for new namespace", err)
			}
		}
	}
	if colHeader.EOF {
		if rcr, ok := demux.outs[demux.currentNamespace].(*RegularCollectionReceiver); ok {
			rcr.err = io.EOF
		}
		demux.outs[demux.currentNamespace].Close()
		demux.NamespaceStatus[demux.currentNamespace] = NamespaceClosed
		length := int64(demux.lengths[demux.currentNamespace])
		crcUInt64, ok := demux.outs[demux.currentNamespace].Sum64()
		if ok {
			crc := int64(crcUInt64)
			if crc != colHeader.CRC {
				return fmt.Errorf("CRC mismatch for namespace %v, %v!=%v",
					demux.currentNamespace,
					crc,
					colHeader.CRC,
				)
			}
			log.Logvf(log.DebugHigh,
				"demux checksum for namespace %v is correct (%v), %v bytes",
				demux.currentNamespace, crc, length)
		} else {
			log.Logvf(log.DebugHigh,
				"demux checksum for namespace %v was not calculated.",
				demux.currentNamespace)
		}
		delete(demux.outs, demux.currentNamespace)
		delete(demux.lengths, demux.currentNamespace)
		// in case we get a BSONBody with this block,
		// we want to ensure that that causes an error
		demux.currentNamespace = ""
	}
	return nil
}

// End is part of the ParserConsumer interface and receives the end of archive notification.
func (demux *Demultiplexer) End() error {
	log.Logvf(log.DebugHigh, "demux End")
	var err error
	if len(demux.outs) != 0 {
		openNss := []string{}
		for ns := range demux.outs {
			openNss = append(openNss, ns)
			if rcr, ok := demux.outs[ns].(*RegularCollectionReceiver); ok {
				rcr.err = newError("archive io error")
			}
			demux.outs[ns].Close()
		}
		err = newError(fmt.Sprintf("archive finished but contained files were unfinished (%v)", openNss))
	} else {
		for ns, status := range demux.NamespaceStatus {
			if status != NamespaceClosed {
				err = newError(fmt.Sprintf("archive finished before all collections were seen (%v)", ns))
			}
		}
	}

	if demux.NamespaceChan != nil {
		close(demux.NamespaceChan)
	}

	return err
}

// BodyBSON is part of the ParserConsumer interface and receives BSON bodies from the parser.
// Its main role is to dispatch the body to the Read() function of the current DemuxOut.
func (demux *Demultiplexer) BodyBSON(buf []byte) error {
	if demux.currentNamespace == "" {
		return newError("collection data without a collection header")
	}

	demux.lengths[demux.currentNamespace] += int64(len(buf))

	out, ok := demux.outs[demux.currentNamespace]
	if !ok {
		return newError("no demux consumer currently consuming namespace " + demux.currentNamespace)
	}
	_, err := out.Write(buf)
	return err
}

// Open installs the DemuxOut as the handler for data for the namespace ns
func (demux *Demultiplexer) Open(ns string, out DemuxOut) {
	// In the current implementation where this is either called before the demultiplexing is running
	// or while the demutiplexer is inside of the NamespaceChan NamespaceErrorChan conversation
	// I think that we don't need to lock outs, but I suspect that if the implementation changes
	// we may need to lock when outs is accessed
	log.Logvf(log.DebugHigh, "demux Open")
	if demux.outs == nil {
		demux.outs = make(map[string]DemuxOut)
		demux.lengths = make(map[string]int64)
	}
	demux.outs[ns] = out
	demux.lengths[ns] = 0
}

// RegularCollectionReceiver implements the intents.file interface.
type RegularCollectionReceiver struct {
	pos              int64 // updated atomically, aligned at the beginning of the struct
	readLenChan      chan int
	readBufChan      chan []byte
	Intent           *intents.Intent
	Origin           string
	Demux            *Demultiplexer
	partialReadArray []byte
	partialReadBuf   []byte
	hash             hash.Hash64
	closeOnce        sync.Once
	openOnce         sync.Once
	err              error
}

func (receiver *RegularCollectionReceiver) Sum64() (uint64, bool) {
	return receiver.hash.Sum64(), true
}

// Read() runs in the restoring goroutine
func (receiver *RegularCollectionReceiver) Read(r []byte) (int, error) {
	if receiver.partialReadBuf != nil && len(receiver.partialReadBuf) > 0 {
		wLen := len(receiver.partialReadBuf)
		copyLen := copy(r, receiver.partialReadBuf)
		if wLen == copyLen {
			receiver.partialReadBuf = nil
		} else {
			receiver.partialReadBuf = receiver.partialReadBuf[copyLen:]
		}
		atomic.AddInt64(&receiver.pos, int64(copyLen))
		return copyLen, nil
	}
	// Since we're the "reader" here, not the "writer" we need to start with a read, in case the chan is closed
	wLen, ok := <-receiver.readLenChan
	if !ok {
		close(receiver.readBufChan)
		return 0, receiver.err
	}
	if wLen > db.MaxBSONSize {
		return 0, fmt.Errorf("incomming buffer size is too big %v", wLen)
	}
	rLen := len(r)
	if wLen > rLen {
		// if the incomming write size is larger then the incomming read buffer then we need to accept
		// the write in a larger buffer, fill the read buffer, then cache the remainder
		receiver.partialReadBuf = receiver.partialReadArray[:wLen]
		receiver.readBufChan <- receiver.partialReadBuf
		writtenLength := <-receiver.readLenChan
		if wLen != writtenLength {
			return 0, fmt.Errorf("regularCollectionReceiver didn't send what it said it would")
		}
		receiver.hash.Write(receiver.partialReadBuf)
		copy(r, receiver.partialReadBuf)
		receiver.partialReadBuf = receiver.partialReadBuf[rLen:]
		atomic.AddInt64(&receiver.pos, int64(rLen))
		return rLen, nil
	}
	// Send the read buff to the BodyBSON ParserConsumer to fill
	receiver.readBufChan <- r
	// Receiver the wLen of data written
	wLen = <-receiver.readLenChan
	receiver.hash.Write(r[:wLen])
	atomic.AddInt64(&receiver.pos, int64(wLen))
	return wLen, nil
}

func (receiver *RegularCollectionReceiver) Pos() int64 {
	return atomic.LoadInt64(&receiver.pos)
}

// Open is part of the intents.file interface.  It creates the chan's in the
// RegularCollectionReceiver and adds the RegularCollectionReceiver to the set of
// RegularCollectonReceivers in the demultiplexer
func (receiver *RegularCollectionReceiver) Open() error {
	// TODO move this implementation to some non intents.file method, to be called from prioritizer.Get
	// So that we don't have to enable this double open stuff.
	// Currently the open needs to finish before the prioritizer.Get finishes, so we open the intents.file
	// in prioritizer.Get even though it's going to get opened again in DumpIntent.
	receiver.openOnce.Do(func() {
		receiver.readLenChan = make(chan int)
		receiver.readBufChan = make(chan []byte)
		receiver.hash = crc64.New(crc64.MakeTable(crc64.ECMA))
		receiver.Demux.Open(receiver.Origin, receiver)
	})
	return nil
}

func (receiver *RegularCollectionReceiver) TakeIOBuffer(ioBuf []byte) {
	receiver.partialReadArray = ioBuf

}
func (receiver *RegularCollectionReceiver) ReleaseIOBuffer() {
	receiver.partialReadArray = nil
}

// Write is part of the DemuxOut interface.
func (receiver *RegularCollectionReceiver) Write(buf []byte) (int, error) {
	//  As a writer, we need to write first, so that the reader can properly detect EOF
	//  Additionally, the reader needs to know the write size, so that it can give us a
	//  properly sized buffer. Sending the incomming buffersize fills both of these needs.
	receiver.readLenChan <- len(buf)
	// Receive from the reader a buffer to put the bytes into
	readBuf := <-receiver.readBufChan
	if len(readBuf) < len(buf) {
		return 0, fmt.Errorf("readbuf is not large enough for incoming BodyBSON (%v<%v)",
			len(readBuf), len(buf))
	}
	copy(readBuf, buf)
	// Send back the length of the data copied in to the buffer
	receiver.readLenChan <- len(buf)
	return len(buf), nil
}

// Close is part of the DemuxOut as well as the intents.file interface. It only closes the readLenChan, as that is what will
// cause the RegularCollectionReceiver.Read() to receive EOF
// Close will get called twice, once in the demultiplexer, and again when the restore goroutine is done with its intent.file
func (receiver *RegularCollectionReceiver) Close() error {
	receiver.closeOnce.Do(func() {
		close(receiver.readLenChan)
		// make sure that we don't return until any reader has finished
		<-receiver.readBufChan
	})
	return nil
}

// SpecialCollectionCache implemnts both DemuxOut as well as intents.file
type SpecialCollectionCache struct {
	pos    int64 // updated atomically, aligned at the beginning of the struct
	Intent *intents.Intent
	Demux  *Demultiplexer
	buf    bytes.Buffer
	hash   hash.Hash64
}

func NewSpecialCollectionCache(intent *intents.Intent, demux *Demultiplexer) *SpecialCollectionCache {
	return &SpecialCollectionCache{
		Intent: intent,
		Demux:  demux,
		hash:   crc64.New(crc64.MakeTable(crc64.ECMA)),
	}
}

// Open is part of the both interfaces, and it does nothing
func (cache *SpecialCollectionCache) Open() error {
	return nil
}

// Close is part of the both interfaces, and it does nothing
func (cache *SpecialCollectionCache) Close() error {
	cache.Intent.Size = int64(cache.buf.Len())
	return nil
}

func (cache *SpecialCollectionCache) Read(p []byte) (int, error) {
	n, err := cache.buf.Read(p)
	atomic.AddInt64(&cache.pos, int64(n))
	return n, err
}

func (cache *SpecialCollectionCache) Pos() int64 {
	return atomic.LoadInt64(&cache.pos)
}

func (cache *SpecialCollectionCache) Write(b []byte) (int, error) {
	cache.hash.Write(b)
	return cache.buf.Write(b)
}

func (cache *SpecialCollectionCache) Sum64() (uint64, bool) {
	return cache.hash.Sum64(), true
}

// MutedCollection implements both DemuxOut as well as intents.file. It serves as a way to
// let the demutiplexer ignore certain embedded streams
type MutedCollection struct {
	Intent *intents.Intent
	Demux  *Demultiplexer
}

// Read is part of the intents.file interface, and does nothing
func (*MutedCollection) Read([]byte) (int, error) {
	// Read is part of the intents.file interface, and does nothing
	return 0, io.EOF
}

// Write is part of the intents.file interface, and does nothing
func (*MutedCollection) Write(b []byte) (int, error) {
	return len(b), nil
}

// Close is part of the intents.file interface, and does nothing
func (*MutedCollection) Close() error {
	return nil
}

// Open is part of the intents.file interface, and does nothing
func (*MutedCollection) Open() error {
	return nil
}

// Sum64 is part of the DemuxOut interface
func (*MutedCollection) Sum64() (uint64, bool) {
	return 0, false
}

//===== Archive Manager Prioritizer =====

// NewPrioritizer careates a new Prioritizer and hooks up its Namespace channels to the ones in demux
func (demux *Demultiplexer) NewPrioritizer(mgr *intents.Manager) *Prioritizer {
	return &Prioritizer{
		NamespaceChan:      demux.NamespaceChan,
		NamespaceErrorChan: demux.NamespaceErrorChan,
		mgr:                mgr,
	}
}

// Prioritizer is a completely reactive prioritizer
// Intents are handed out as they arrive in the archive
type Prioritizer struct {
	NamespaceChan      <-chan string
	NamespaceErrorChan chan<- error
	mgr                *intents.Manager
}

// Get waits for a new namespace from the NamespaceChan, and returns a Intent found for it
func (prioritizer *Prioritizer) Get() *intents.Intent {
	namespace, ok := <-prioritizer.NamespaceChan
	if !ok {
		return nil
	}
	intent := prioritizer.mgr.IntentForNamespace(namespace)
	if intent == nil {
		prioritizer.NamespaceErrorChan <- fmt.Errorf("no intent for namespace %v", namespace)
	} else {
		if intent.BSONFile != nil {
			intent.BSONFile.Open()
		}
		if intent.IsOplog() {
			// once we see the oplog we
			// cause the RestoreIntents to finish because we don't
			// want RestoreIntents to restore the oplog
			prioritizer.NamespaceErrorChan <- io.EOF
			return nil
		}
		prioritizer.NamespaceErrorChan <- nil
	}
	return intent
}

// Finish is part of the IntentPrioritizer interface, and does nothing
func (prioritizer *Prioritizer) Finish(*intents.Intent) {
	// no-op
	return
}
