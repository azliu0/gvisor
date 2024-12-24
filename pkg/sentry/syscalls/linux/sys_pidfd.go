// Copyright 2024 The gVisor Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

package linux

import (
	"gvisor.dev/gvisor/pkg/abi/linux"
	"gvisor.dev/gvisor/pkg/errors/linuxerr"
	"gvisor.dev/gvisor/pkg/sentry/arch"
	"gvisor.dev/gvisor/pkg/sentry/kernel"
	"gvisor.dev/gvisor/pkg/sentry/fsimpl/pidfd"
)

// PidfdOpen implements Linux syscall pidfd_open(2).
func PidfdOpen(t *kernel.Task, sysno uintptr, args arch.SyscallArguments) (uintptr, *kernel.SyscallControl, error) {
	pid := kernel.ThreadID(args[0].Int())
	flags := args[1].Int()
	
	fd, err := pidfd_open(t, pid, flags)
	if err != nil {
		return 0, nil, err
	}
	
	return uintptr(fd), nil, nil
}

func pidfd_open(t *kernel.Task, pid kernel.ThreadID, flags int32) (int32, error) {
	if flags & ^linux.O_NONBLOCK != 0 {
		return 0, linuxerr.EINVAL
	}

	if pid <= 0 {
		return 0, linuxerr.EINVAL
	}

	targetTask := t.PIDNamespace().TaskWithID(pid)
	if targetTask == nil {
		return 0, linuxerr.ESRCH
	}

	file, err := pidfd.New(targetTask, uint32(flags))
	if err != nil {
		return 0, err
	}
	defer file.DecRef(t)

	fd, err := t.NewFDFrom(0, file, kernel.FDFlags{
		CloseOnExec: true,
	})
	if err != nil {
		return 0, err
	}

	return fd, nil
}
