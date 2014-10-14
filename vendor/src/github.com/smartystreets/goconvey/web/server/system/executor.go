package system

import "os/exec"

type Executor interface {
	Execute(directory, name string, arguments ...string) (output string, err error)
}

type CommandExecutor struct{}

func (self *CommandExecutor) Execute(directory, name string, arguments ...string) (output string, err error) {
	command := exec.Command(name, arguments...)
	command.Dir = directory
	rawOutput, err := command.CombinedOutput()
	output = string(rawOutput)
	return
}

func NewCommandExecutor() *CommandExecutor {
	return new(CommandExecutor)
}
