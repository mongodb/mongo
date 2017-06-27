// +build sasl

package options

func init() {
	ConnectionOptFunctions = append(ConnectionOptFunctions, registerGSSAPIOptions)
}

func registerGSSAPIOptions(self *ToolOptions) error {
	_, err := self.parser.AddGroup("kerberos options", "", self.Kerberos)
	if err != nil {
		return err
	}
	self.URI.AddKnownURIParameters(KnownURIOptionsKerberos)
	BuiltWithGSSAPI = true
	return nil
}
