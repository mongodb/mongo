// +build ssl

package options

func init() {
	OptionRegistrationFunctions = append(OptionRegistrationFunctions, registerSSLOptions)
}

func registerSSLOptions(self *ToolOptions) error {
	_, err := self.parser.AddGroup("ssl options", "", self.SSL)
	return err
}
