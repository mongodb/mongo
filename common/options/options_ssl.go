// +build ssl

package options

func init() {
	ConnectionOptFunctions = append(ConnectionOptFunctions, registerSSLOptions)
}

func registerSSLOptions(self *ToolOptions) error {
	_, err := self.parser.AddGroup("ssl options", "", self.SSL)
	return err
}
