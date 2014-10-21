package testutil

import (
	"fmt"
	commonopts "github.com/mongodb/mongo-tools/common/options"
	"os"
	"runtime"
)

var (
	WINDOWS_KERBEROS_PASSWORD_ENV = "MONGODB_KERBEROS_PASSWORD"
)

func GetKerberosOptions() *commonopts.ToolOptions {
	opts := &commonopts.ToolOptions{
		Namespace: &commonopts.Namespace{
			DB:         "kerberos",
			Collection: "test",
		},
		SSL: &commonopts.SSL{},
		Auth: &commonopts.Auth{
			Username:  "drivers@LDAPTEST.10GEN.CC",
			Source:    "$external",
			Mechanism: "GSSAPI",
		},
		Kerberos: &commonopts.Kerberos{},
		Connection: &commonopts.Connection{
			Host: "ldaptest.10gen.cc",
			Port: "27017",
		},
	}

	if runtime.GOOS == "windows" {
		opts.Auth.Password = os.Getenv(WINDOWS_KERBEROS_PASSWORD_ENV)
		if opts.Auth.Password == "" {
			panic(fmt.Sprintf("Need to set %v environment variable to run kerberos tests on windows",
				WINDOWS_KERBEROS_PASSWORD_ENV))
		}
	}

	return opts
}
