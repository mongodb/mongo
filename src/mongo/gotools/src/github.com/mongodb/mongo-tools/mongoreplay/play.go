package mongoreplay

import (
	"compress/gzip"
	"fmt"
	"io"
	"os"
	"time"

	"github.com/10gen/llmgo/bson"
)

// PlayCommand stores settings for the mongoreplay 'play' subcommand
type PlayCommand struct {
	GlobalOpts *Options `no-flag:"true"`
	StatOptions
	PlaybackFile string  `description:"path to the playback file to play from" short:"p" long:"playback-file" required:"yes"`
	Speed        float64 `description:"multiplier for playback speed (1.0 = real-time, .5 = half-speed, 3.0 = triple-speed, etc.)" long:"speed" default:"1.0"`
	URL          string  `short:"h" long:"host" description:"Location of the host to play back against" default:"mongodb://localhost:27017"`
	Repeat       int     `long:"repeat" description:"Number of times to play the playback file" default:"1"`
	QueueTime    int     `long:"queueTime" description:"don't queue ops much further in the future than this number of seconds" default:"15"`
	NoPreprocess bool    `long:"no-preprocess" description:"don't preprocess the input file to premap data such as mongo cursorIDs"`
	Gzip         bool    `long:"gzip" description:"decompress gzipped input"`
	Collect      string  `long:"collect" description:"Stat collection format; 'format' option uses the --format string" choice:"json" choice:"format" choice:"none" default:"none"`
}

const queueGranularity = 1000

// NewOpChanFromFile runs a goroutine that will read and unmarshal recorded ops
// from a file and push them in to a recorded op chan. Any errors encountered
// are pushed to an error chan. Both the recorded op chan and the error chan are
// returned by the function.
// The error chan won't be readable until the recorded op chan gets closed.
func NewOpChanFromFile(file *PlaybackFileReader, repeat int) (<-chan *RecordedOp, <-chan error) {
	ch := make(chan *RecordedOp)
	e := make(chan error)

	var last time.Time
	var first time.Time
	var loopDelta time.Duration
	go func() {
		defer close(e)
		e <- func() error {
			defer close(ch)
			toolDebugLogger.Logv(Info, "Beginning tapefile read")
			for generation := 0; generation < repeat; generation++ {
				_, err := file.Seek(0, 0)
				if err != nil {
					return fmt.Errorf("PlaybackFile Seek: %v", err)
				}

				var order int64
				for {
					recordedOp, err := file.NextRecordedOp()
					if err != nil {
						if err == io.EOF {
							break
						}
						return err
					}
					last = recordedOp.Seen.Time
					if first.IsZero() {
						first = recordedOp.Seen.Time
					}
					recordedOp.Seen.Time = recordedOp.Seen.Add(loopDelta)
					recordedOp.Generation = generation
					recordedOp.Order = order
					// We want to suppress EOF's unless you're in the last
					// generation because all of the ops for one connection
					// across different generations get executed in the same
					// session. We don't want to close the session until the
					// connection closes in the last generation.
					if !recordedOp.EOF || generation == repeat-1 {
						ch <- recordedOp
					}
					order++
				}
				toolDebugLogger.Logvf(DebugHigh, "generation: %v", generation)
				loopDelta += last.Sub(first)
				first = time.Time{}
				continue
			}
			return io.EOF
		}()
	}()
	return ch, e
}

// GzipReadSeeker wraps an io.ReadSeeker for gzip reading
type GzipReadSeeker struct {
	readSeeker io.ReadSeeker
	*gzip.Reader
}

// NewGzipReadSeeker initializes a new GzipReadSeeker
func NewGzipReadSeeker(rs io.ReadSeeker) (*GzipReadSeeker, error) {
	gzipReader, err := gzip.NewReader(rs)
	if err != nil {
		return nil, err
	}
	return &GzipReadSeeker{rs, gzipReader}, nil
}

// Seek sets the offset for the next Read, and can only seek to the
// beginning of the file.
func (g *GzipReadSeeker) Seek(offset int64, whence int) (int64, error) {
	if whence != 0 || offset != 0 {
		return 0, fmt.Errorf("GzipReadSeeker can only seek to beginning of file")
	}
	_, err := g.readSeeker.Seek(offset, whence)
	if err != nil {
		return 0, err
	}
	g.Reset(g.readSeeker)
	return 0, nil
}

// PlaybackFileReader stores the necessary information for a playback source,
// which is just an io.ReadCloser.
type PlaybackFileReader struct {
	io.ReadSeeker
}

// NewPlaybackFileReader initializes a new PlaybackFileReader
func NewPlaybackFileReader(filename string, gzip bool) (*PlaybackFileReader, error) {
	var readSeeker io.ReadSeeker

	readSeeker, err := os.Open(filename)
	if err != nil {
		return nil, err
	}

	if gzip {
		readSeeker, err = NewGzipReadSeeker(readSeeker)
		if err != nil {
			return nil, err
		}
	}

	return &PlaybackFileReader{readSeeker}, nil
}

// NextRecordedOp iterates through the PlaybackFileReader to yield the next
// RecordedOp. It returns io.EOF when successfully complete.
func (file *PlaybackFileReader) NextRecordedOp() (*RecordedOp, error) {
	buf, err := ReadDocument(file)
	if err != nil {
		if err != io.EOF {
			err = fmt.Errorf("ReadDocument Error: %v", err)
		}
		return nil, err
	}
	doc := new(RecordedOp)
	err = bson.Unmarshal(buf, doc)
	if err != nil {
		return nil, fmt.Errorf("Unmarshal RecordedOp Error: %v\n", err)
	}

	return doc, nil
}

// ValidateParams validates the settings described in the PlayCommand struct.
func (play *PlayCommand) ValidateParams(args []string) error {
	switch {
	case len(args) > 0:
		return fmt.Errorf("unknown argument: %s", args[0])
	case play.Speed <= 0:
		return fmt.Errorf("Invalid setting for --speed: '%v'", play.Speed)
	case play.Repeat < 1:
		return fmt.Errorf("Invalid setting for --repeat: '%v', value must be >=1", play.Repeat)
	}
	return nil
}

// Execute runs the program for the 'play' subcommand
func (play *PlayCommand) Execute(args []string) error {
	err := play.ValidateParams(args)
	if err != nil {
		return err
	}
	play.GlobalOpts.SetLogging()

	statColl, err := newStatCollector(play.StatOptions, play.Collect, true, true)
	if err != nil {
		return err
	}
	userInfoLogger.Logvf(Always, "Doing playback at %.2fx speed", play.Speed)

	playbackFileReader, err := NewPlaybackFileReader(play.PlaybackFile, play.Gzip)
	if err != nil {
		return err
	}

	context := NewExecutionContext(statColl)

	var opChan <-chan *RecordedOp
	var errChan <-chan error

	if !play.NoPreprocess {
		opChan, errChan = NewOpChanFromFile(playbackFileReader, 1)

		preprocessMap, err := newPreprocessCursorManager(opChan)

		if err != nil {
			return fmt.Errorf("PreprocessMap: %v", err)
		}

		err = <-errChan
		if err != io.EOF {
			return fmt.Errorf("OpChan: %v", err)
		}

		_, err = playbackFileReader.Seek(0, 0)
		if err != nil {
			return err
		}
		context.CursorIDMap = preprocessMap
	}

	opChan, errChan = NewOpChanFromFile(playbackFileReader, play.Repeat)

	if err := Play(context, opChan, play.Speed, play.URL, play.Repeat, play.QueueTime); err != nil {
		userInfoLogger.Logvf(Always, "Play: %v\n", err)
	}

	//handle the error from the errchan
	err = <-errChan
	if err != nil && err != io.EOF {
		userInfoLogger.Logvf(Always, "OpChan: %v", err)
	}
	return nil
}

// Play is responsible for playing ops from a RecordedOp channel to the
// given url.
func Play(context *ExecutionContext,
	opChan <-chan *RecordedOp,
	speed float64,
	url string,
	repeat int,
	queueTime int) error {

	sessionChans := make(map[int64]chan<- *RecordedOp)
	var playbackStartTime, recordingStartTime time.Time
	var connectionID int64
	var opCounter int
	for op := range opChan {
		opCounter++
		if op.Seen.IsZero() {
			return fmt.Errorf("Can't play operation found with zero-timestamp: %#v", op)
		}
		if recordingStartTime.IsZero() {
			recordingStartTime = op.Seen.Time
			playbackStartTime = time.Now()
		}

		// opDelta is the difference in time between when the file's recording
		// began and and when this particular op is played. For the first
		// operation in the playback, it's 0.
		opDelta := op.Seen.Sub(recordingStartTime)

		// Adjust the opDelta for playback by dividing it by playback speed setting;
		// e.g. 2x speed means the delta is half as long.
		scaledDelta := float64(opDelta) / (speed)
		op.PlayAt = &PreciseTime{playbackStartTime.Add(time.Duration(int64(scaledDelta)))}

		// Every queueGranularity ops make sure that we're no more then
		// QueueTime seconds ahead Which should mean that the maximum that we're
		// ever ahead is QueueTime seconds of ops + queueGranularity more ops.
		// This is so that when we're at QueueTime ahead in the playback file we
		// don't sleep after every read, and generally read and queue
		// queueGranularity number of ops at a time and then sleep until the
		// last read op is QueueTime ahead.
		if opCounter%queueGranularity == 0 {
			toolDebugLogger.Logvf(DebugHigh, "Waiting to prevent excess buffering with opCounter: %v", opCounter)
			time.Sleep(op.PlayAt.Add(time.Duration(-queueTime) * time.Second).Sub(time.Now()))
		}

		sessionChan, ok := sessionChans[op.SeenConnectionNum]
		if !ok {
			connectionID++
			sessionChan = context.newExecutionSession(url, op.PlayAt.Time, connectionID)
			sessionChans[op.SeenConnectionNum] = sessionChan
		}
		if op.EOF {
			userInfoLogger.Logv(DebugLow, "EOF Seen in playback")
			close(sessionChan)
			delete(sessionChans, op.SeenConnectionNum)
		} else {
			sessionChan <- op
		}
	}
	for connectionNum, sessionChan := range sessionChans {
		close(sessionChan)
		delete(sessionChans, connectionNum)
	}
	toolDebugLogger.Logvf(Info, "Waiting for sessions to finish")
	context.SessionChansWaitGroup.Wait()

	context.StatCollector.Close()
	toolDebugLogger.Logvf(Always, "%v ops played back in %v seconds over %v connections", opCounter, time.Now().Sub(playbackStartTime), connectionID)
	if repeat > 1 {
		toolDebugLogger.Logvf(Always, "%v ops per generation for %v generations", opCounter/repeat, repeat)
	}
	return nil
}
