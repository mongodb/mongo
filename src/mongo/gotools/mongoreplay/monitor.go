package mongoreplay

import (
	"fmt"
	"io"
	"os"
	"os/signal"
	"syscall"
	"time"
)

// MonitorCommand stores settings for the mongoreplay 'monitor' subcommand
type MonitorCommand struct {
	GlobalOpts *Options `no-flag:"true"`
	StatOptions
	OpStreamSettings
	PairedMode   bool   `long:"paired" description:"Output only one line for a request/reply pair"`
	Gzip         bool   `long:"gzip" description:"decompress gzipped input"`
	PlaybackFile string `short:"p" description:"path to playback file to read from" long:"playback-file"`
}

// UnresolvedOpInfo holds information about an op
type UnresolvedOpInfo struct {
	Stat     *OpStat
	ParsedOp Op
	Op       *RecordedOp
}

// AddUnresolvedOp takes an operation that is supposed to receive a reply and
// keeps it around so that its latency can be calculated using the incoming
// reply.
func (gen *RegularStatGenerator) AddUnresolvedOp(op *RecordedOp, parsedOp Op, requestStat *OpStat) {
	gen.UnresolvedOps[opKey{
		driverEndpoint: op.SrcEndpoint,
		serverEndpoint: op.DstEndpoint,
		opID:           op.Header.RequestID,
	}] = UnresolvedOpInfo{
		Stat:     requestStat,
		Op:       op,
		ParsedOp: parsedOp,
	}
}

// ResolveOp generates an OpStat from the pairing of a request with its reply.
// When running in paired mode ResolveOp returns an OpStat which contains the
// payload from both the request and the reply. Otherwise, it returns an OpStat
// with just the data from the reply along with the latency between the request
// and the reply.
//
// recordedReply is the just received reply in the form of a RecordedOp, which
// contains additional metadata.  parsedReply is the same reply, parsed so that
// the payload of the op can be accesssed.  replyStat is the OpStat created by
// the GenerateOpStat function, containing computed metadata about the reply.
func (gen *RegularStatGenerator) ResolveOp(recordedReply *RecordedOp, reply Replyable, replyStat *OpStat) *OpStat {
	result := &OpStat{}

	key := opKey{
		driverEndpoint: recordedReply.DstEndpoint,
		serverEndpoint: recordedReply.SrcEndpoint,
		opID:           recordedReply.Header.ResponseTo,
	}
	originalOpInfo, foundOriginal := gen.UnresolvedOps[key]
	if !foundOriginal {
		return replyStat
	}

	if gen.PairedMode {
		// When in paired mode, the result of 'resolving' is a complete request
		// reply pair.  therefore, 'result' is set to be the original OpStat
		// generated for the request and the reply data is added in in either
		// case
		result = originalOpInfo.Stat
	} else {
		// When in unpaired mode, the result of 'resolving' is data about the
		// reply, along with its latency. Therefore, 'result' is set to be the
		// reply and data about the original op is largely ignored.
		result = replyStat
	}
	result.Errors = reply.getErrors()
	result.NumReturned = reply.getNumReturned()
	result.ReplyData = replyStat.ReplyData
	result.LatencyMicros = int64(replyStat.Seen.Sub(*originalOpInfo.Stat.Seen) / (time.Microsecond))
	delete(gen.UnresolvedOps, key)

	return result
}

// Execute runs the program for the 'monitor' subcommand
func (monitor *MonitorCommand) Execute(args []string) error {
	monitor.GlobalOpts.SetLogging()
	monitor.ValidateParams(args)

	var opChan <-chan *RecordedOp
	var errChan <-chan error
	if monitor.PlaybackFile != "" {
		playbackFileReader, err := NewPlaybackFileReader(monitor.PlaybackFile, monitor.Gzip)
		if err != nil {
			return err
		}
		opChan, errChan = NewOpChanFromFile(playbackFileReader, 1)

	} else {
		ctx, err := getOpstream(monitor.OpStreamSettings)
		if err != nil {
			return err
		}
		opChan = ctx.mongoOpStream.Ops
		e := make(chan error)
		errChan = e
		go func() {
			defer close(e)
			if err := ctx.packetHandler.Handle(ctx.mongoOpStream, -1); err != nil {
				e <- fmt.Errorf("monitor: error handling packet stream: %s", err)
			}
		}()
		// When a signal is received to kill the process, stop the packet
		// handler so we gracefully flush all ops being processed before
		// exiting.
		sigChan := make(chan os.Signal, 1)
		signal.Notify(sigChan, syscall.SIGTERM, syscall.SIGINT, syscall.SIGHUP)
		go func() {
			// Block until a signal is received.
			s := <-sigChan
			toolDebugLogger.Logvf(Info, "Got signal %v, closing PCAP handle", s)
			ctx.packetHandler.Close()
		}()
	}
	statColl, err := newStatCollector(monitor.StatOptions, monitor.PairedMode, false)
	if err != nil {
		return err
	}
	defer statColl.Close()

	for op := range opChan {
		parsedOp, err := op.RawOp.Parse()
		if err != nil {
			return err
		}
		statColl.Collect(op, parsedOp, nil, "")
	}
	err = <-errChan
	if err != nil && err != io.EOF {
		userInfoLogger.Logvf(Always, "OpChan: %v", err)
	}
	return nil
}

// ValidateParams validates the settings described in the MonitorCommand struct.
func (monitor *MonitorCommand) ValidateParams(args []string) error {
	numInputTypes := 0

	if monitor.PcapFile != "" {
		if monitor.Gzip {
			return fmt.Errorf("incompatible options: pcap file and gzip")
		}
		numInputTypes++
	}
	if monitor.NetworkInterface != "" {
		if monitor.Gzip {
			return fmt.Errorf("incompatible options: network interface and gzip")
		}
		numInputTypes++
	}
	if monitor.PlaybackFile != "" {
		numInputTypes++
		if monitor.Expression != "" {
			return fmt.Errorf("incompatible options: tape file with a filter expression")
		}
	}
	switch {
	case len(args) > 0:
		return fmt.Errorf("unknown argument: %s", args[0])
	case numInputTypes < 1:
		return fmt.Errorf("must specify one input source")
	case numInputTypes > 1:
		return fmt.Errorf("must not specify more than one input")
	}

	if monitor.OpStreamSettings.PacketBufSize == 0 {
		// default heap size
		if monitor.OpStreamSettings.NetworkInterface != "" {
			monitor.OpStreamSettings.PacketBufSize = 1
		} else {
			monitor.OpStreamSettings.PacketBufSize = 1000
		}
	}

	return nil
}
