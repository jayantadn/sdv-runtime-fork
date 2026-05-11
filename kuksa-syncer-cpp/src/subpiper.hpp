// Copyright (c) 2025 Eclipse Foundation.
//
// SPDX-License-Identifier: MIT

#pragma once

#include <atomic>
#include <functional>
#include <list>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace sdv {

// ---------------------------------------------------------------------------
// SubPiper
//
// Launches a child process with separate stdout / stderr capture.  Mirrors
// the Python subpiper module:
//
//   stdout_callback(master_id, line)   — called for each stdout line
//   stderr_callback(master_id, line)   — called for each stderr line
//   finished_callback(master_id, retcode) — called when process exits
//
// If finished_callback is provided the call to start() is non-blocking and
// the object manages the child lifetime in background threads.
// ---------------------------------------------------------------------------

using LineCallback     = std::function<void(const std::string& masterId,
                                            const std::string& line)>;
using FinishedCallback = std::function<void(const std::string& masterId,
                                            int retcode)>;

class SubPiper {
public:
    SubPiper(std::string              cmd,
             std::string              masterId,
             LineCallback             stdoutCb  = nullptr,
             LineCallback             stderrCb  = nullptr,
             FinishedCallback         finishedCb = nullptr);

    ~SubPiper();

    // Start the child process.
    // Non-blocking when a finished_callback was provided.
    void start();

    // Send SIGKILL to the child (best-effort; no-op if already exited).
    void kill();

    // True after the process has been started.
    bool isRunning() const;

    pid_t pid() const { return pid_; }

private:
    std::string      cmd_;
    std::string      masterId_;
    LineCallback     stdoutCb_;
    LineCallback     stderrCb_;
    FinishedCallback finishedCb_;

    pid_t            pid_     = -1;
    int              stdoutFd_ = -1;
    int              stderrFd_ = -1;
    std::atomic_bool running_{ false };

    std::thread      stdoutThread_;
    std::thread      stderrThread_;
    std::thread      waitThread_;

    void readPipe(int fd, const LineCallback& cb);
    void waitForExit();
    void closeFds();
};

} // namespace sdv
