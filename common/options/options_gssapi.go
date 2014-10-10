// +build sasl

package options

func init() {
	OptionRegistrationFunctions = append(OptionRegistrationFunctions, registerGSSAPIOptions)
}

func registerGSSAPIOptions(self *ToolOptions) error {
	_, err := self.parser.AddGroup("kerberos options", "", self.Kerberos)
	return err
}
