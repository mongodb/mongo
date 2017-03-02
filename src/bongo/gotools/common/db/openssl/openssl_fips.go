// +build ssl
// +build -darwin

package openssl

import "github.com/spacemonkeygo/openssl"

func init() { sslInitializationFunctions = append(sslInitializationFunctions, SetUpFIPSMode) }

func SetUpFIPSMode(opts *ToolOptions) error {
	if err := openssl.FIPSModeSet(opts.SSLFipsMode); err != nil {
		return fmt.Errorf("couldn't set FIPS mode to %v: %v", opts.SSLFipsMode, err)
	}
	return nil
}
