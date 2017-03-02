// +build !failpoints

package options

// EnableFailpoints removes the failpoints options
func EnableFailpoints(opts *ToolOptions) {
	opt := opts.FindOptionByLongName("failpoints")
	opt.LongName = ""
}
