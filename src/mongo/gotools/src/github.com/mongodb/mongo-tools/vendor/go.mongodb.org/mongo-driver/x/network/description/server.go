// Copyright (C) MongoDB, Inc. 2017-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package description

import (
	"fmt"
	"time"

	"go.mongodb.org/mongo-driver/bson/primitive"
	"go.mongodb.org/mongo-driver/tag"
	"go.mongodb.org/mongo-driver/x/network/address"
	"go.mongodb.org/mongo-driver/x/network/result"
)

// UnsetRTT is the unset value for a round trip time.
const UnsetRTT = -1 * time.Millisecond

// SelectedServer represents a selected server that is a member of a topology.
type SelectedServer struct {
	Server
	Kind TopologyKind
}

// Server represents a description of a server. This is created from an isMaster
// command.
type Server struct {
	Addr address.Address

	AverageRTT            time.Duration
	AverageRTTSet         bool
	Compression           []string // compression methods returned by server
	CanonicalAddr         address.Address
	ElectionID            primitive.ObjectID
	HeartbeatInterval     time.Duration
	LastError             error
	LastUpdateTime        time.Time
	LastWriteTime         time.Time
	MaxBatchCount         uint32
	MaxDocumentSize       uint32
	MaxMessageSize        uint32
	Members               []address.Address
	ReadOnly              bool
	SessionTimeoutMinutes uint32
	SetName               string
	SetVersion            uint32
	Tags                  tag.Set
	Kind                  ServerKind
	WireVersion           *VersionRange

	SaslSupportedMechs []string // user-specific from server handshake
}

// NewServer creates a new server description from the given parameters.
func NewServer(addr address.Address, isMaster result.IsMaster) Server {
	i := Server{
		Addr: addr,

		CanonicalAddr:         address.Address(isMaster.Me).Canonicalize(),
		Compression:           isMaster.Compression,
		ElectionID:            isMaster.ElectionID,
		LastUpdateTime:        time.Now().UTC(),
		LastWriteTime:         isMaster.LastWriteTimestamp,
		MaxBatchCount:         isMaster.MaxWriteBatchSize,
		MaxDocumentSize:       isMaster.MaxBSONObjectSize,
		MaxMessageSize:        isMaster.MaxMessageSizeBytes,
		SaslSupportedMechs:    isMaster.SaslSupportedMechs,
		SessionTimeoutMinutes: isMaster.LogicalSessionTimeoutMinutes,
		SetName:               isMaster.SetName,
		SetVersion:            isMaster.SetVersion,
		Tags:                  tag.NewTagSetFromMap(isMaster.Tags),
	}

	if i.CanonicalAddr == "" {
		i.CanonicalAddr = addr
	}

	if isMaster.OK != 1 {
		i.LastError = fmt.Errorf("not ok")
		return i
	}

	for _, host := range isMaster.Hosts {
		i.Members = append(i.Members, address.Address(host).Canonicalize())
	}

	for _, passive := range isMaster.Passives {
		i.Members = append(i.Members, address.Address(passive).Canonicalize())
	}

	for _, arbiter := range isMaster.Arbiters {
		i.Members = append(i.Members, address.Address(arbiter).Canonicalize())
	}

	i.Kind = Standalone

	if isMaster.IsReplicaSet {
		i.Kind = RSGhost
	} else if isMaster.SetName != "" {
		if isMaster.IsMaster {
			i.Kind = RSPrimary
		} else if isMaster.Hidden {
			i.Kind = RSMember
		} else if isMaster.Secondary {
			i.Kind = RSSecondary
		} else if isMaster.ArbiterOnly {
			i.Kind = RSArbiter
		} else {
			i.Kind = RSMember
		}
	} else if isMaster.Msg == "isdbgrid" {
		i.Kind = Mongos
	}

	i.WireVersion = &VersionRange{
		Min: isMaster.MinWireVersion,
		Max: isMaster.MaxWireVersion,
	}

	return i
}

// SetAverageRTT sets the average round trip time for this server description.
func (s Server) SetAverageRTT(rtt time.Duration) Server {
	s.AverageRTT = rtt
	if rtt == UnsetRTT {
		s.AverageRTTSet = false
	} else {
		s.AverageRTTSet = true
	}

	return s
}

// DataBearing returns true if the server is a data bearing server.
func (s Server) DataBearing() bool {
	return s.Kind == RSPrimary ||
		s.Kind == RSSecondary ||
		s.Kind == Mongos ||
		s.Kind == Standalone
}

// SelectServer selects this server if it is in the list of given candidates.
func (s Server) SelectServer(_ Topology, candidates []Server) ([]Server, error) {
	for _, candidate := range candidates {
		if candidate.Addr == s.Addr {
			return []Server{candidate}, nil
		}
	}
	return nil, nil
}
