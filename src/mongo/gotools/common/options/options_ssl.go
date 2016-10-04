// +build ssl

package options

import "github.com/spacemonkeygo/openssl"

func init() {
	ConnectionOptFunctions = append(ConnectionOptFunctions, registerSSLOptions)
	versionInfos = append(versionInfos, versionInfo{
		key:   "OpenSSL version",
		value: openssl.Version,
	})
}

func registerSSLOptions(self *ToolOptions) error {
	_, err := self.parser.AddGroup("ssl options", "", self.SSL)
	return err
}
