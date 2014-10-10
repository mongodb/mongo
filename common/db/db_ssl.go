// +build ssl

package db

import (
	"github.com/mongodb/mongo-tools/common/db/openssl"
	"github.com/mongodb/mongo-tools/common/options"
)

func init() {
	GetConnectorFuncs = append(GetConnectorFuncs, getSSLConnector)
}

// Get the right type of connector, based on the options
func getSSLConnector(opts options.ToolOptions) DBConnector {
	if opts.SSL.UseSSL {
		return &openssl.SSLDBConnector{}
	}
	return nil
}
