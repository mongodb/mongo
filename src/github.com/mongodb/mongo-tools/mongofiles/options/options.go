package options

type StorageOptions struct {
	// LocalFileName is an option that specifies what filename to use for (put|get)
	LocalFileName string `long:"local" short:"l" description:"local filename for put|get (default is to use the same name as 'gridfs filename')"`

	// ContentType is an option that specifies the Content/MIME type to use for 'put'
	ContentType string `long:"type" short:"t" description:"Content/MIME type for put (default is to omit)"`

	// Replace if set will remove other files with same name after 'put'
	Replace bool `long:"replace" short:"r" description:"Remove other files with same name after put"`
}

func (self *StorageOptions) Name() string {
	return "storage"
}
