package mongoreplay

import (
	"compress/gzip"
	"fmt"
	"io"
	"os"
	"os/signal"
	"syscall"

	"github.com/10gen/llmgo/bson"
	"github.com/google/gopacket/pcap"
)

// RecordCommand stores settings for the mongoreplay 'record' subcommand
type RecordCommand struct {
	GlobalOpts *Options `no-flag:"true"`
	OpStreamSettings
	Gzip         bool   `long:"gzip" description:"compress output file with Gzip"`
	FullReplies  bool   `long:"full-replies" description:"save full reply payload in playback file"`
	PlaybackFile string `short:"p" description:"path to playback file to record to" long:"playback-file" required:"yes"`
}

// ErrPacketsDropped means that some packets were dropped
type ErrPacketsDropped struct {
	Count int
}

func (e ErrPacketsDropped) Error() string {
	return fmt.Sprintf("completed with %d packets dropped", e.Count)
}

type packetHandlerContext struct {
	packetHandler *PacketHandler
	mongoOpStream *MongoOpStream
	pcapHandle    *pcap.Handle
}

func getOpstream(cfg OpStreamSettings) (*packetHandlerContext, error) {
	if cfg.PacketBufSize < 1 {
		return nil, fmt.Errorf("invalid packet buffer size")
	}

	var pcapHandle *pcap.Handle
	var err error
	if len(cfg.PcapFile) > 0 {
		pcapHandle, err = pcap.OpenOffline(cfg.PcapFile)
		if err != nil {
			return nil, fmt.Errorf("error opening pcap file: %v", err)
		}
	} else if len(cfg.NetworkInterface) > 0 {
		pcapHandle, err = pcap.OpenLive(cfg.NetworkInterface, 32*1024*1024, false, pcap.BlockForever)
		if err != nil {
			return nil, fmt.Errorf("error listening to network interface: %v", err)
		}
	} else {
		return nil, fmt.Errorf("must specify either a pcap file or network interface to record from")
	}

	if len(cfg.Expression) > 0 {
		err = pcapHandle.SetBPFFilter(cfg.Expression)
		if err != nil {
			return nil, fmt.Errorf("error setting packet filter expression: %v", err)
		}
	}

	h := NewPacketHandler(pcapHandle)
	h.Verbose = userInfoLogger.isInVerbosity(DebugLow)

	toolDebugLogger.Logvf(Info, "Created packet buffer size %d", cfg.PacketBufSize)
	m := NewMongoOpStream(cfg.PacketBufSize)
	return &packetHandlerContext{h, m, pcapHandle}, nil
}

// PlaybackWriter stores the necessary information for a playback destination,
// which is an io.WriteCloser and its location.
type PlaybackWriter struct {
	io.WriteCloser
	fname string
}

// NewPlaybackWriter initializes a new PlaybackWriter
func NewPlaybackWriter(playbackFileName string, isGzipWriter bool) (*PlaybackWriter, error) {
	pbWriter := &PlaybackWriter{
		fname: playbackFileName,
	}
	toolDebugLogger.Logvf(DebugLow, "Opening playback file %v", playbackFileName)
	file, err := os.Create(pbWriter.fname)
	if err != nil {
		return nil, fmt.Errorf("error opening playback file to write to: %v", err)
	}
	if isGzipWriter {
		pbWriter.WriteCloser = gzip.NewWriter(file)
	} else {
		pbWriter.WriteCloser = file
	}
	return pbWriter, nil
}

// ValidateParams validates the settings described in the RecordCommand struct.
func (record *RecordCommand) ValidateParams(args []string) error {
	switch {
	case len(args) > 0:
		return fmt.Errorf("unknown argument: %s", args[0])
	case record.PcapFile != "" && record.NetworkInterface != "":
		return fmt.Errorf("must only specify an interface or a pcap file")
	}
	if record.OpStreamSettings.PacketBufSize == 0 {
		// default heap size
		record.OpStreamSettings.PacketBufSize = 1000
	}
	return nil
}

// Execute runs the program for the 'record' subcommand
func (record *RecordCommand) Execute(args []string) error {
	err := record.ValidateParams(args)
	if err != nil {
		return err
	}
	record.GlobalOpts.SetLogging()

	ctx, err := getOpstream(record.OpStreamSettings)
	if err != nil {
		return err
	}

	// When a signal is received to kill the process, stop the packet handler so
	// we gracefully flush all ops being processed before exiting.
	sigChan := make(chan os.Signal, 1)
	signal.Notify(sigChan, syscall.SIGTERM, syscall.SIGINT, syscall.SIGHUP)
	go func() {
		// Block until a signal is received.
		s := <-sigChan
		toolDebugLogger.Logvf(Info, "Got signal %v, closing PCAP handle", s)
		ctx.packetHandler.Close()
	}()
	playbackWriter, err := NewPlaybackWriter(record.PlaybackFile, record.Gzip)
	if err != nil {
		return err
	}

	return Record(ctx, playbackWriter, record.FullReplies)

}

// Record writes pcap data into a playback file
func Record(ctx *packetHandlerContext,
	playbackWriter *PlaybackWriter,
	noShortenReply bool) error {

	ch := make(chan error)
	go func() {
		defer close(ch)
		for op := range ctx.mongoOpStream.Ops {
			if (op.Header.OpCode == OpCodeReply || op.Header.OpCode == OpCodeCommandReply) &&
				!noShortenReply {
				op.ShortenReply()
			}
			bsonBytes, err := bson.Marshal(op)
			if err != nil {
				ch <- fmt.Errorf("error marshaling message: %v", err)
				return
			}
			_, err = playbackWriter.Write(bsonBytes)
			if err != nil {
				ch <- fmt.Errorf("error writing message: %v", err)
				return
			}
		}
		ch <- nil
	}()

	if err := ctx.packetHandler.Handle(ctx.mongoOpStream, -1); err != nil {
		return fmt.Errorf("record: error handling packet stream: %s", err)
	}

	stats, err := ctx.pcapHandle.Stats()
	if err != nil {
		toolDebugLogger.Logvf(Always, "Warning: got err %v getting pcap handle stats", err)
	} else {
		toolDebugLogger.Logvf(Info, "PCAP stats: %#v", stats)
	}

	err = <-ch
	if err == nil && stats != nil && stats.PacketsDropped != 0 {
		err = ErrPacketsDropped{stats.PacketsDropped}
	}
	return err
}
