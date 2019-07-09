// Copyright (C) MongoDB, Inc. 2017-present.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may
// not use this file except in compliance with the License. You may obtain
// a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

package description

import (
	"sort"
	"strings"

	"go.mongodb.org/mongo-driver/x/mongo/driver/address"
)

// Topology represents a description of a mongodb topology
type Topology struct {
	Servers               []Server
	Kind                  TopologyKind
	SessionTimeoutMinutes uint32
}

// Server returns the server for the given address. Returns false if the server
// could not be found.
func (t Topology) Server(addr address.Address) (Server, bool) {
	for _, server := range t.Servers {
		if server.Addr.String() == addr.String() {
			return server, true
		}
	}
	return Server{}, false
}

// TopologyDiff is the difference between two different topology descriptions.
type TopologyDiff struct {
	Added   []Server
	Removed []Server
}

// DiffTopology compares the two topology descriptions and returns the difference.
func DiffTopology(old, new Topology) TopologyDiff {
	var diff TopologyDiff

	// TODO: do this without sorting...
	oldServers := serverSorter(old.Servers)
	newServers := serverSorter(new.Servers)

	sort.Sort(oldServers)
	sort.Sort(newServers)

	i := 0
	j := 0
	for {
		if i < len(oldServers) && j < len(newServers) {
			comp := strings.Compare(oldServers[i].Addr.String(), newServers[j].Addr.String())
			switch comp {
			case 1:
				//left is bigger than
				diff.Added = append(diff.Added, newServers[j])
				j++
			case -1:
				// right is bigger
				diff.Removed = append(diff.Removed, oldServers[i])
				i++
			case 0:
				i++
				j++
			}
		} else if i < len(oldServers) {
			diff.Removed = append(diff.Removed, oldServers[i])
			i++
		} else if j < len(newServers) {
			diff.Added = append(diff.Added, newServers[j])
			j++
		} else {
			break
		}
	}

	return diff
}

// HostlistDiff is the difference between a topology and a host list.
type HostlistDiff struct {
	Added   []string
	Removed []string
}

// DiffHostlist compares the topology description and host list and returns the difference.
func (t Topology) DiffHostlist(hostlist []string) HostlistDiff {
	var diff HostlistDiff

	oldServers := serverSorter(t.Servers)
	sort.Sort(oldServers)
	sort.Strings(hostlist)

	i := 0
	j := 0
	for {
		if i < len(oldServers) && j < len(hostlist) {
			oldServer := oldServers[i].Addr.String()
			comp := strings.Compare(oldServer, hostlist[j])
			switch comp {
			case 1:
				// oldServers[i] is bigger
				diff.Added = append(diff.Added, hostlist[j])
				j++
			case -1:
				// hostlist[j] is bigger
				diff.Removed = append(diff.Removed, oldServer)
				i++
			case 0:
				i++
				j++
			}
		} else if i < len(oldServers) {
			diff.Removed = append(diff.Removed, oldServers[i].Addr.String())
			i++
		} else if j < len(hostlist) {
			diff.Added = append(diff.Added, hostlist[j])
			j++
		} else {
			break
		}
	}

	return diff
}

type serverSorter []Server

func (ss serverSorter) Len() int      { return len(ss) }
func (ss serverSorter) Swap(i, j int) { ss[i], ss[j] = ss[j], ss[i] }
func (ss serverSorter) Less(i, j int) bool {
	return strings.Compare(ss[i].Addr.String(), ss[j].Addr.String()) < 0
}
