package testutil

import (
	"fmt"
	"github.com/mongodb/mongo-tools/common/options"
	"os"
	"runtime"
)

var (
	WINDOWS_KERBEROS_PASSWORD_ENV = "MONGODB_KERBEROS_PASSWORD"
)

func GetKerberosOptions() (*options.ToolOptions, error) {
	opts := &options.ToolOptions{
		Namespace: &options.Namespace{
			DB:         "kerberos",
			Collection: "test",
		},
		SSL: &options.SSL{},
		Auth: &options.Auth{
			Username:  "drivers@LDAPTEST.10GEN.CC",
			Source:    "$external",
			Mechanism: "GSSAPI",
		},
		Kerberos: &options.Kerberos{},
		Connection: &options.Connection{
			Host: "ldaptest.10gen.cc",
			Port: "27017",
		},
	}

	if runtime.GOOS == "windows" {
		opts.Auth.Password = os.Getenv(WINDOWS_KERBEROS_PASSWORD_ENV)
		if opts.Auth.Password == "" {
			return nil, fmt.Errorf("Need to set %v environment variable to run "+
				"kerberos tests on windows", WINDOWS_KERBEROS_PASSWORD_ENV)
		}
	}

	return opts, nil
}
