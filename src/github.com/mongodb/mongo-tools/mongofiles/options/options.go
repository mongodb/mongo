package options

type StorageOptions struct {
	// Local is an option that specifies what filename to use for (put|get)
	Local string `long:"local" short:"l" description:"local filename for put|get (default is to use the same name as 'gridfs filename')"`

	// Type is an option that specifies the MIME type to use for 'put'
	Type string `long:"type" short:"t" description:"MIME type for put (default is to omit)"`

	// Replace if set will remove other files with same name after 'put'
	Replace bool `long:"replace" short:"r" description:"Remove other files with same name after put"`
}

func (self *StorageOptions) Name() string {
	return "storage"
}
