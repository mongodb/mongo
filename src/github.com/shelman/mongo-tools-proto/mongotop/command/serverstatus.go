package command

import ()

type ServerStatus struct{}

// implement dat interface
func (self *ServerStatus) AsRunnable() interface{} {
	return "serverStatus"
}

// implement dat other interface
func (self *ServerStatus) Diff(other Command) (Diff, error) {

	return nil, nil
}
