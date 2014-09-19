package db

import (
	"github.com/mongodb/mongo-tools/common/db/kerberos"
	"github.com/mongodb/mongo-tools/common/options"
)

func init() {
	GetConnectorFuncs = append(GetConnectorFuncs, getGSSAPIConnector)
}

func getGSSAPIConnector(opts options.ToolOptions) DBConnector {
	if opts.Auth.Mechanism == "GSSAPI" {
		return &kerberos.KerberosDBConnector{}
	}
	return nil
}
