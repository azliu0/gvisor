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

// Package pidfd implements process fds.
package pidfd

import (
	"gvisor.dev/gvisor/pkg/abi/linux"
	"gvisor.dev/gvisor/pkg/context"
	"gvisor.dev/gvisor/pkg/sentry/kernel"
	"gvisor.dev/gvisor/pkg/sentry/vfs"
	"gvisor.dev/gvisor/pkg/waiter"
)

// ProcessFileDescription implements vfs.FileDescriptionImpl for pidfds.
//
// +stateify savable
type ProcessFileDescription struct {
	vfsfd vfs.FileDescription
	vfs.FileDescriptionDefaultImpl
	vfs.DentryMetadataFileDescriptionImpl
	vfs.NoLockFD

	tid      kernel.ThreadID
	k        *kernel.Kernel
	pidns    *kernel.PIDNamespace
	nonblock bool
}

// New creates a new process fd.
func New(task *kernel.Task, flags uint32) (*vfs.FileDescription, error) {
	fd := &ProcessFileDescription{
		tid:      task.ThreadID(),
		k:        task.Kernel(),
		pidns:    task.PIDNamespace(),
		nonblock: flags&linux.O_NONBLOCK != 0,
	}

	fileFlags := uint32(linux.O_RDWR)
	if flags&linux.O_NONBLOCK != 0 {
		fileFlags |= linux.O_NONBLOCK
	}

	vd := task.Kernel().VFS().NewAnonVirtualDentry("[pidfd]")
	defer vd.DecRef(task)

	if err := fd.vfsfd.Init(fd, fileFlags, vd.Mount(), vd.Dentry(), &vfs.FileDescriptionOptions{
		UseDentryMetadata: true,
	}); err != nil {
		return nil, err
	}

	return &fd.vfsfd, nil
}

// PID returns the process ID associated with this pidfd.
func (pfd *ProcessFileDescription) PID() kernel.ThreadID {
	return pfd.tid
}

// Nonblocking returns whether this pidfd is nonblocking.
func (pfd *ProcessFileDescription) Nonblocking() bool {
	return pfd.nonblock
}

// TaskExited returns whether the task associated with this pidfd has exited.
func (pfd *ProcessFileDescription) TaskExited() bool {
	if task := pfd.pidns.TaskWithID(pfd.tid); task != nil {
		return task.ExitState() != kernel.TaskExitNone
	}
	return true
}

// Release implements vfs.FileDescriptionImpl.Release.
func (pfd *ProcessFileDescription) Release(context.Context) {
}

// Readiness implements waiter.Waitable.Readiness.
func (pfd *ProcessFileDescription) Readiness(mask waiter.EventMask) waiter.EventMask {
	ready := waiter.EventMask(0)
	if pfd.TaskExited() {
		ready |= waiter.ReadableEvents
	}
	return mask & ready
}

// EventRegister implements waiter.Waitable.EventRegister.
func (pfd *ProcessFileDescription) EventRegister(e *waiter.Entry) error {
	if task := pfd.pidns.TaskWithID(pfd.tid); task != nil {
		task.PidfdEventRegister(e)
	}
	return nil
}

// EventUnregister implements waiter.Waitable.EventUnregister.
func (pfd *ProcessFileDescription) EventUnregister(e *waiter.Entry) {
	if task := pfd.pidns.TaskWithID(pfd.tid); task != nil {
		task.PidfdEventUnregister(e)
	}
}

// Epollable implements FileDescriptionImpl.Epollable.
func (pfd *ProcessFileDescription) Epollable() bool {
	return true
}
