package util

import (
	"fmt"
	"strings"
)

// Split the host string into the individual nodes to connect to, appending the
// port if necessary.
func CreateConnectionAddrs(host, port string) []string {

	// parse the host string into the individual hosts
	addrs := parseHost(host)

	// if a port is specified, append it to all the hosts
	if port != "" {
		for idx, addr := range addrs {
			addrs[idx] = fmt.Sprintf("%v:%v", addr, port)
		}
	}

	return addrs
}

// Helper function for parsing the host string into addresses.  Returns a slice
// of the individual addresses to use to connect.
func parseHost(host string) []string {

	// strip off the replica set name from the beginning
	slashIndex := strings.Index(host, "/")
	if slashIndex != -1 {
		if slashIndex == len(host)-1 {
			return []string{""}
		}
		host = host[slashIndex+1:]
	}

	// split into the individual hosts
	return strings.Split(host, ",")
}
