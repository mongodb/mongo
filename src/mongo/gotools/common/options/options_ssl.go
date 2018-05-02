// +build ssl

package options

func init() {
	ConnectionOptFunctions = append(ConnectionOptFunctions, registerSSLOptions)
}

func registerSSLOptions(self *ToolOptions) error {
	_, err := self.parser.AddGroup("ssl options", "", self.SSL)
	if err != nil {
		return err
	}
	if self.enabledOptions.URI {
		self.URI.AddKnownURIParameters(KnownURIOptionsSSL)
	}
	BuiltWithSSL = true
	return nil
}
