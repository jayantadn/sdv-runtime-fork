// Copyright (c) 2025 Eclipse Foundation.
//
// SPDX-License-Identifier: MIT

#include "subpiper.hpp"

#include <array>
#include <csignal>
#include <cstring>
#include <iostream>
#include <sstream>
#include <stdexcept>

#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace sdv {

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

SubPiper::SubPiper(std::string   cmd,
                   std::string   masterId,
                   LineCallback  stdoutCb,
                   LineCallback  stderrCb,
                   FinishedCallback finishedCb)
    : cmd_(std::move(cmd))
    , masterId_(std::move(masterId))
    , stdoutCb_(std::move(stdoutCb))
    , stderrCb_(std::move(stderrCb))
    , finishedCb_(std::move(finishedCb))
{}

SubPiper::~SubPiper() {
    kill();
    if (stdoutThread_.joinable()) stdoutThread_.join();
    if (stderrThread_.joinable()) stderrThread_.join();
    if (waitThread_.joinable())   waitThread_.join();
    closeFds();
}

// ---------------------------------------------------------------------------
// Start
// ---------------------------------------------------------------------------

void SubPiper::start() {
    if (running_.load()) return;

    // Create pipes: pipeOut[0]=read end, pipeOut[1]=write end
    int pipeOut[2], pipeErr[2];
    if (::pipe(pipeOut) < 0 || ::pipe(pipeErr) < 0) {
        throw std::runtime_error(std::string("pipe() failed: ") +
                                 ::strerror(errno));
    }

    pid_ = ::fork();
    if (pid_ < 0) {
        throw std::runtime_error(std::string("fork() failed: ") +
                                 ::strerror(errno));
    }

    if (pid_ == 0) {
        // ── Child ──────────────────────────────────────────────────────────
        ::close(pipeOut[0]);
        ::close(pipeErr[0]);
        ::dup2(pipeOut[1], STDOUT_FILENO);
        ::dup2(pipeErr[1], STDERR_FILENO);
        ::close(pipeOut[1]);
        ::close(pipeErr[1]);

        // Execute via shell so that the command string can include arguments,
        // pipes, redirects, etc. – mirroring Python's subprocess with shell=False
        // but split via shlex.
        ::execl("/bin/sh", "sh", "-c", cmd_.c_str(), nullptr);

        // If execl returns, it failed
        std::cerr << "execl failed: " << ::strerror(errno) << std::endl;
        ::_exit(127);
    }

    // ── Parent ──────────────────────────────────────────────────────────────
    ::close(pipeOut[1]);
    ::close(pipeErr[1]);

    stdoutFd_ = pipeOut[0];
    stderrFd_ = pipeErr[0];
    running_.store(true);

    // Launch reader threads
    stdoutThread_ = std::thread([this] { readPipe(stdoutFd_, stdoutCb_); });
    stderrThread_ = std::thread([this] { readPipe(stderrFd_, stderrCb_); });

    if (finishedCb_) {
        // Non-blocking: background wait thread
        waitThread_ = std::thread([this] { waitForExit(); });
    } else {
        // Blocking: wait inline, then join readers
        waitForExit();
        if (stdoutThread_.joinable()) stdoutThread_.join();
        if (stderrThread_.joinable()) stderrThread_.join();
    }
}

// ---------------------------------------------------------------------------
// Kill
// ---------------------------------------------------------------------------

void SubPiper::kill() {
    if (pid_ > 0 && running_.load()) {
        ::kill(pid_, SIGKILL);
    }
}

bool SubPiper::isRunning() const { return running_.load(); }

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

void SubPiper::readPipe(int fd, const LineCallback& cb) {
    if (fd < 0) return;

    char        buf[4096];
    std::string partial;

    while (true) {
        ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n <= 0) break;

        partial.append(buf, static_cast<size_t>(n));

        // Emit complete lines
        size_t pos = 0;
        size_t nl;
        while ((nl = partial.find('\n', pos)) != std::string::npos) {
            std::string line = partial.substr(pos, nl - pos);
            // Strip trailing \r
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (cb) cb(masterId_, line);
            pos = nl + 1;
        }
        partial.erase(0, pos);
    }

    // Emit any remaining partial line
    if (!partial.empty() && cb) cb(masterId_, partial);
}

void SubPiper::waitForExit() {
    int status = 0;
    ::waitpid(pid_, &status, 0);
    running_.store(false);
    closeFds();

    int retcode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    if (finishedCb_) finishedCb_(masterId_, retcode);
}

void SubPiper::closeFds() {
    if (stdoutFd_ >= 0) { ::close(stdoutFd_); stdoutFd_ = -1; }
    if (stderrFd_ >= 0) { ::close(stderrFd_); stderrFd_ = -1; }
}

} // namespace sdv
