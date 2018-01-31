// Copyright (C) MongoDB, Inc. 2014-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package mongoreplay

import (
	"fmt"
	"os"
	"os/signal"
	"syscall"

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
		inactive, err := pcap.NewInactiveHandle(cfg.NetworkInterface)
		if err != nil {
			return nil, fmt.Errorf("error creating a pcap handle: %v", err)
		}
		// This is safe; calling `Activate()` steals the underlying ptr.
		defer inactive.CleanUp()

		err = inactive.SetSnapLen(64 * 1024)
		if err != nil {
			return nil, fmt.Errorf("error setting snaplen on pcap handle: %v", err)
		}

		err = inactive.SetPromisc(false)
		if err != nil {
			return nil, fmt.Errorf("error setting promisc on pcap handle: %v", err)
		}

		err = inactive.SetTimeout(pcap.BlockForever)
		if err != nil {
			return nil, fmt.Errorf("error setting timeout on pcap handle: %v", err)
		}

		// CaptureBufSize is in KiB to match units on `tcpdump -B`.
		err = inactive.SetBufferSize(cfg.CaptureBufSize * 1024)
		if err != nil {
			return nil, fmt.Errorf("error setting buffer size on pcap handle: %v", err)
		}

		pcapHandle, err = inactive.Activate()
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
	assemblerOptions := AssemblerOptions{
		MaxBufferedPagesTotal: cfg.MaxBufferedPages,
	}

	h := NewPacketHandler(pcapHandle, assemblerOptions)
	h.Verbose = userInfoLogger.isInVerbosity(DebugLow)

	toolDebugLogger.Logvf(Info, "Created packet buffer size %d", cfg.PacketBufSize)
	m := NewMongoOpStream(cfg.PacketBufSize)
	return &packetHandlerContext{h, m, pcapHandle}, nil
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
	if record.OpStreamSettings.CaptureBufSize == 0 {
		// default capture buffer size to 2 MiB (same as libpcap)
		record.OpStreamSettings.CaptureBufSize = 2 * 1024
	}
	if record.OpStreamSettings.MaxBufferedPages < 0 {
		return fmt.Errorf("bufferedPagesMax cannot be less than 0")
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
	playbackFileWriter, err := NewPlaybackFileWriter(record.PlaybackFile, false, record.Gzip)
	if err != nil {
		return err
	}
	defer playbackFileWriter.Close()

	return Record(ctx, playbackFileWriter, record.FullReplies)

}

// Record writes pcap data into a playback file
func Record(ctx *packetHandlerContext,
	playbackWriter *PlaybackFileWriter,
	noShortenReply bool) error {

	ch := make(chan error)
	go func() {
		defer close(ch)
		var fail error
		for op := range ctx.mongoOpStream.Ops {
			// since we don't currently have a way to shutdown packetHandler.Handle()
			// continue to read from ctx.mongoOpStream.Ops even after a faltal error
			if fail != nil {
				toolDebugLogger.Logvf(DebugHigh, "not recording op because of record error %v", fail)
				continue
			}
			if (op.Header.OpCode == OpCodeReply || op.Header.OpCode == OpCodeCommandReply) &&
				!noShortenReply {
				err := op.ShortenReply()
				if err != nil {
					userInfoLogger.Logvf(DebugLow, "stream %v problem shortening reply: %v", op.SeenConnectionNum, err)
					continue
				}
			}
			err := bsonToWriter(playbackWriter, op)
			if err != nil {
				fail = fmt.Errorf("error writing message: %v", err)
				userInfoLogger.Logvf(Always, "%v", err)
				continue
			}
		}
		ch <- fail
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
