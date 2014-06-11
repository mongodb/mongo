package command

import ()

type Command interface {
	AsRunnable() interface{}
}
