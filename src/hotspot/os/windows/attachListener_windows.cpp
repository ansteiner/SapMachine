/*
 * Copyright (c) 2005, 2024, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#include "precompiled.hpp"
#include "logging/log.hpp"
#include "runtime/interfaceSupport.inline.hpp"
#include "runtime/os.hpp"
#include "services/attachListener.hpp"

#include <windows.h>
#include <signal.h>             // SIGBREAK
#include <stdio.h>

// The AttachListener thread services a queue of operation requests. It blocks in the dequeue
// function until a request is enqueued. A client enqueues a request by creating
// a thread in this process using the Win32 CreateRemoteThread function. That thread
// executes a small stub generated by the client. The stub invokes the
// JVM_EnqueueOperation or JVM_EnqueueOperation_v2 function which checks the operation parameters
// and enqueues the operation request to the queue. The thread created by
// the client is a native thread and is restricted to a single page of stack. To keep
// it simple operation requests are pre-allocated at initialization time. An enqueue thus
// takes a preallocated request, populates the operation parameters, adds it to
// queue and wakes up the attach listener.
//
// Differences between Attach API v1 and v2:
// In v1 (jdk6+) client calls JVM_EnqueueOperation function and passes all operation parameters
// as arguments of the function.
// In v2 (jdk24+) client calls JVM_EnqueueOperation_v2 function and passes only pipe name.
// Attach listeners connects to the pipe (in read/write mode) and reads all operation parameters
// (the same way as other platform implementations read them using sockets).
//
// When an operation has completed the attach listener is required to send the
// operation result and any result data to the client. In this implementation the
// client is a pipe server. In the enqueue operation it provides the name of pipe
// to this process. When the operation is completed this process opens the pipe and
// sends the result and output back to the client. Note that writing to the pipe
// (and flushing the output) is a blocking operation. This means that a non-responsive
// client could potentially hang the attach listener thread indefinitely. In that
// case no new operations would be executed but the VM would continue as normal.
// As only suitably privileged processes can open this process we concluded that
// this wasn't worth worrying about.


class PipeChannel : public AttachOperation::RequestReader, public AttachOperation::ReplyWriter {
private:
  HANDLE _hPipe;
public:
  PipeChannel() : _hPipe(INVALID_HANDLE_VALUE) {}
  ~PipeChannel() {
    close();
  }

  bool opened() const {
    return _hPipe != INVALID_HANDLE_VALUE;
  }

  bool open(const char* pipe, bool write_only) {
    _hPipe = ::CreateFile(pipe,
                          GENERIC_WRITE | (write_only ? 0 : GENERIC_READ),
                          0,              // no sharing
                          nullptr,        // default security attributes
                          OPEN_EXISTING,  // opens existing pipe
                          0,              // default attributes
                          nullptr);       // no template file
    if (_hPipe == INVALID_HANDLE_VALUE) {
      log_error(attach)("could not open %s (%d) pipe %s",
                        (write_only ? "write-only" : "read-write"), GetLastError(), pipe);
      return false;
    }
    return true;
  }

  void close() {
    if (opened()) {
      CloseHandle(_hPipe);
      _hPipe = INVALID_HANDLE_VALUE;
    }
  }

  // RequestReader
  int read(void* buffer, int size) override {
    assert(opened(), "must be");
    DWORD nread;
    BOOL fSuccess = ReadFile(_hPipe,
                             buffer,
                             (DWORD)size,
                             &nread,
                             nullptr);   // not overlapped
    if (!fSuccess) {
      log_error(attach)("pipe read error (%d)", GetLastError());
      return -1;
    }
    return (int)nread;
  }

  // ReplyWriter
  int write(const void* buffer, int size) override {
    assert(opened(), "must be");
    DWORD written;
    BOOL fSuccess = WriteFile(_hPipe,
                              buffer,
                              (DWORD)size,
                              &written,
                              nullptr);  // not overlapped
    if (!fSuccess) {
        log_error(attach)("pipe write error (%d)", GetLastError());
        return -1;
    }
    return (int)written;
  }

  void flush() override {
    assert(opened(), "must be");
    FlushFileBuffers(_hPipe);
  }
};

class Win32AttachOperation: public AttachOperation {
public:
  enum {
    pipe_name_max = 256             // maximum pipe name
  };

private:
  PipeChannel _pipe;

public:
  // for v1 pipe must be write-only
  bool open_pipe(const char* pipe_name, bool write_only) {
    return _pipe.open(pipe_name, write_only);
  }

  bool read_request() {
    return AttachOperation::read_request(&_pipe);
  }

public:
  void complete(jint result, bufferedStream* result_stream) override;
};


// Win32AttachOperationRequest is an element of AttachOperation request list.
class Win32AttachOperationRequest {
private:
  AttachAPIVersion _ver;
  char _name[AttachOperation::name_length_max + 1];
  char _arg[AttachOperation::arg_count_max][AttachOperation::arg_length_max + 1];
  char _pipe[Win32AttachOperation::pipe_name_max + 1];

  Win32AttachOperationRequest* _next;

  void set_value(char* dst, const char* str, size_t dst_size) {
    if (str != nullptr) {
        assert(strlen(str) < dst_size, "exceeds maximum length");
        strncpy(dst, str, dst_size - 1);
        dst[dst_size - 1] = '\0';
    } else {
      strcpy(dst, "");
    }
  }

public:
  void set(AttachAPIVersion ver, const char* pipename,
           const char* cmd = nullptr,
           const char* arg0 = nullptr,
           const char* arg1 = nullptr,
           const char* arg2 = nullptr) {
      _ver = ver;
      set_value(_name, cmd, sizeof(_name));
      set_value(_arg[0], arg0, sizeof(_arg[0]));
      set_value(_arg[1], arg1, sizeof(_arg[1]));
      set_value(_arg[2], arg2, sizeof(_arg[2]));
      set_value(_pipe, pipename, sizeof(_pipe));
  }
  AttachAPIVersion ver() const {
    return _ver;
  }
  const char* cmd() const {
    return _name;
  }
  const char* arg(int i) const {
    return (i >= 0 && i < AttachOperation::arg_count_max) ? _arg[i] : nullptr;
  }
  const char* pipe() const {
    return _pipe;
  }

  Win32AttachOperationRequest* next() const {
    return _next;
  }
  void set_next(Win32AttachOperationRequest* next) {
    _next = next;
  }

  // noarg constructor as operation is preallocated
  Win32AttachOperationRequest() {
    set(ATTACH_API_V1, "<nopipe>");
    set_next(nullptr);
  }
};


class Win32AttachListener: AllStatic {
 private:
  enum {
    max_enqueued_operations = 4
  };

  // protects the preallocated list and the operation list
  static HANDLE _mutex;

  // head of preallocated operations list
  static Win32AttachOperationRequest* _avail;

  // head and tail of enqueue operations list
  static Win32AttachOperationRequest* _head;
  static Win32AttachOperationRequest* _tail;


  static Win32AttachOperationRequest* head()                       { return _head; }
  static void set_head(Win32AttachOperationRequest* head)          { _head = head; }

  static Win32AttachOperationRequest* tail()                       { return _tail; }
  static void set_tail(Win32AttachOperationRequest* tail)          { _tail = tail; }


  // A semaphore is used for communication about enqueued operations.
  // The maximum count for the semaphore object will be set to "max_enqueued_operations".
  // The state of a semaphore is signaled when its count is greater than
  // zero (there are operations enqueued), and nonsignaled when it is zero.
  static HANDLE _enqueued_ops_semaphore;
  static HANDLE enqueued_ops_semaphore() { return _enqueued_ops_semaphore; }

 public:
  enum {
    ATTACH_ERROR_DISABLED               = 100,              // error codes
    ATTACH_ERROR_RESOURCE               = 101,
    ATTACH_ERROR_ILLEGALARG             = 102,
    ATTACH_ERROR_INTERNAL               = 103
  };

  static int init();
  static HANDLE mutex()                                     { return _mutex; }

  static Win32AttachOperationRequest* available()                  { return _avail; }
  static void set_available(Win32AttachOperationRequest* avail)    { _avail = avail; }

  // enqueue an operation to the end of the list
  static int enqueue(AttachAPIVersion ver, const char* cmd,
      const char* arg1, const char* arg2, const char* arg3, const char* pipename);

  // dequeue an operation from from head of the list
  static Win32AttachOperation* dequeue();
};

// statics
HANDLE Win32AttachListener::_mutex;
HANDLE Win32AttachListener::_enqueued_ops_semaphore;
Win32AttachOperationRequest* Win32AttachListener::_avail;
Win32AttachOperationRequest* Win32AttachListener::_head;
Win32AttachOperationRequest* Win32AttachListener::_tail;


// Preallocate the maximum number of operations that can be enqueued.
int Win32AttachListener::init() {
  _mutex = (void*)::CreateMutex(nullptr, FALSE, nullptr);
  guarantee(_mutex != (HANDLE)nullptr, "mutex creation failed");

  _enqueued_ops_semaphore = ::CreateSemaphore(nullptr, 0, max_enqueued_operations, nullptr);
  guarantee(_enqueued_ops_semaphore != (HANDLE)nullptr, "semaphore creation failed");

  set_head(nullptr);
  set_tail(nullptr);
  set_available(nullptr);

  for (int i=0; i<max_enqueued_operations; i++) {
    Win32AttachOperationRequest* op = new Win32AttachOperationRequest();
    op->set_next(available());
    set_available(op);
  }

  AttachListener::set_supported_version(ATTACH_API_V2);

  return 0;
}

// Enqueue an operation. This is called from a native thread that is not attached to VM.
// Also we need to be careful not to execute anything that results in more than a 4k stack.
//
int Win32AttachListener::enqueue(AttachAPIVersion ver, const char* cmd,
    const char* arg0, const char* arg1, const char* arg2, const char* pipename) {

  log_debug(attach)("AttachListener::enqueue, ver = %d, cmd = %s", (int)ver, cmd);

  // wait up to 10 seconds for listener to be up and running
  int sleep_count = 0;
  while (!AttachListener::is_initialized()) {
    Sleep(1000); // 1 second
    sleep_count++;
    if (sleep_count > 10) { // try for 10 seconds
      return ATTACH_ERROR_DISABLED;
    }
  }

  // check that all parameters to the operation
  if (strlen(cmd) > AttachOperation::name_length_max) return ATTACH_ERROR_ILLEGALARG;
  if (strlen(arg0) > AttachOperation::arg_length_max) return ATTACH_ERROR_ILLEGALARG;
  if (strlen(arg1) > AttachOperation::arg_length_max) return ATTACH_ERROR_ILLEGALARG;
  if (strlen(arg2) > AttachOperation::arg_length_max) return ATTACH_ERROR_ILLEGALARG;
  if (strlen(pipename) > Win32AttachOperation::pipe_name_max) return ATTACH_ERROR_ILLEGALARG;

  // check for a well-formed pipename
  if (strstr(pipename, "\\\\.\\pipe\\") != pipename) return ATTACH_ERROR_ILLEGALARG;

  // grab the lock for the list
  DWORD res = ::WaitForSingleObject(mutex(), INFINITE);
  if (res != WAIT_OBJECT_0) {
    return ATTACH_ERROR_INTERNAL;
  }

  // try to get an operation from the available list
  Win32AttachOperationRequest* op = available();
  if (op != nullptr) {
    set_available(op->next());

    // add to end (tail) of list
    op->set_next(nullptr);
    if (tail() == nullptr) {
      set_head(op);
    } else {
      tail()->set_next(op);
    }
    set_tail(op);

    op->set(ver, pipename, cmd, arg0, arg1, arg2);

    // Increment number of enqueued operations.
    // Side effect: Semaphore will be signaled and will release
    // any blocking waiters (i.e. the AttachListener thread).
    BOOL not_exceeding_semaphore_maximum_count =
      ::ReleaseSemaphore(enqueued_ops_semaphore(), 1, nullptr);
    guarantee(not_exceeding_semaphore_maximum_count, "invariant");
  }

  ::ReleaseMutex(mutex());

  return (op != nullptr) ? 0 : ATTACH_ERROR_RESOURCE;
}


// dequeue the operation from the head of the operation list.
Win32AttachOperation* Win32AttachListener::dequeue() {
  for (;;) {
    DWORD res = ::WaitForSingleObject(enqueued_ops_semaphore(), INFINITE);
    // returning from WaitForSingleObject will have decreased
    // the current count of the semaphore by 1.
    guarantee(res != WAIT_FAILED,   "WaitForSingleObject failed with error code: %lu", GetLastError());
    guarantee(res == WAIT_OBJECT_0, "WaitForSingleObject failed with return value: %lu", res);

    res = ::WaitForSingleObject(mutex(), INFINITE);
    guarantee(res != WAIT_FAILED,   "WaitForSingleObject failed with error code: %lu", GetLastError());
    guarantee(res == WAIT_OBJECT_0, "WaitForSingleObject failed with return value: %lu", res);

    Win32AttachOperation* op = nullptr;
    Win32AttachOperationRequest* request = head();
    if (request != nullptr) {
      log_debug(attach)("AttachListener::dequeue, got request, ver = %d, cmd = %s", request->ver(), request->cmd());

      set_head(request->next());
      if (head() == nullptr) {     // list is empty
        set_tail(nullptr);
      }

      switch (request->ver()) {
      case ATTACH_API_V1:
        op = new Win32AttachOperation();
        op->set_name(request->cmd());
        for (int i = 0; i < AttachOperation::arg_count_max; i++) {
          op->append_arg(request->arg(i));
        }
        if (!op->open_pipe(request->pipe(), true/*write-only*/)) {
          // error is already logged
          delete op;
          op = nullptr;
        }
        break;
      case ATTACH_API_V2:
        op = new Win32AttachOperation();
        if (!op->open_pipe(request->pipe(), false/*write-only*/)
            || !op->read_request()) {
          // error is already logged
          delete op;
          op = nullptr;
        }
        break;
      default:
        log_error(attach)("AttachListener::dequeue, unsupported version: %d", request->ver(), request->cmd());
        break;
      }
    }
    // put the operation back on the available list
    request->set_next(Win32AttachListener::available());
    Win32AttachListener::set_available(request);

    ::ReleaseMutex(mutex());

    if (op != nullptr) {
      log_debug(attach)("AttachListener::dequeue, return op: %s", op->name());
      return op;
    }
  }
}

void Win32AttachOperation::complete(jint result, bufferedStream* result_stream) {
  JavaThread* thread = JavaThread::current();
  ThreadBlockInVM tbivm(thread);

  write_reply(&_pipe, result, result_stream);

  delete this;
}


// AttachListener functions

AttachOperation* AttachListener::dequeue() {
  JavaThread* thread = JavaThread::current();
  ThreadBlockInVM tbivm(thread);

  AttachOperation* op = Win32AttachListener::dequeue();

  return op;
}

void AttachListener::vm_start() {
  // nothing to do
}

int AttachListener::pd_init() {
  return Win32AttachListener::init();
}

// This function is used for Un*x OSes only.
// We need not to implement it for Windows.
bool AttachListener::check_socket_file() {
  return false;
}

bool AttachListener::init_at_startup() {
  return true;
}

// no trigger mechanism on Windows to start Attach Listener lazily
bool AttachListener::is_init_trigger() {
  return false;
}

void AttachListener::abort() {
  // nothing to do
}

void AttachListener::pd_data_dump() {
  os::signal_notify(SIGBREAK);
}

void AttachListener::pd_detachall() {
  // do nothing for now
}

// Native thread started by remote client executes this.
extern "C" {
  JNIEXPORT jint JNICALL
  JVM_EnqueueOperation(char* cmd, char* arg0, char* arg1, char* arg2, char* pipename) {
    return (jint)Win32AttachListener::enqueue(ATTACH_API_V1, cmd, arg0, arg1, arg2, pipename);
  }

  JNIEXPORT jint JNICALL
  JVM_EnqueueOperation_v2(char* pipename) {
    return (jint)Win32AttachListener::enqueue(ATTACH_API_V2, "", "", "", "", pipename);
  }
} // extern
