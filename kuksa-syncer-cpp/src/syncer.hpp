// Copyright (c) 2025 Eclipse Foundation.
//
// SPDX-License-Identifier: MIT

#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <sio_client.h>
#include <nlohmann/json.hpp>

#include "kuksa_client.hpp"
#include "subpiper.hpp"

namespace sdv {

// ---------------------------------------------------------------------------
// RunnerEntry — one running child application
// ---------------------------------------------------------------------------
struct RunnerEntry {
    std::string              appName;
    std::shared_ptr<SubPiper> runner;
    std::string              requestFrom;
    double                   startTime;   // epoch seconds
};

// ---------------------------------------------------------------------------
// ApiSubscriber — one client subscribed to VSS signal updates
// ---------------------------------------------------------------------------
struct ApiSubscriber {
    double                   registeredAt; // epoch seconds
    std::vector<std::string> apis;
};

// ---------------------------------------------------------------------------
// Syncer
//
// C++ port of kuksa-syncer/syncer.py.
//
//  • Maintains a Socket.IO connection to the Kit Server.
//  • Maintains a gRPC connection to the KUKSA databroker.
//  • Three background ticker threads mirror the Python asyncio tasks:
//      ticker_fast  – reads subscribed VSS signals every 0.3 s
//      ticker       – cleanup of stale runners / subscribers every 1 s
//      ticker_5s    – reports runtime state every 5 s
// ---------------------------------------------------------------------------
class Syncer {
public:
    // Default paths / ports can be overridden via environment variables or ctor
    Syncer();
    ~Syncer();

    // Connect to KUKSA broker and Socket.IO server, then run forever.
    // Blocks until stop() is called or the connection is lost.
    void run();

    // Request a graceful shutdown.
    void stop();

private:
    // ── Configuration ───────────────────────────────────────────────────────
    std::string brokerHost_;
    int         brokerPort_;
    std::string serverUrl_;
    std::string clientId_;
    std::string mockSignalPath_;

    static constexpr double kTimeToKeepSubscriberAlive = 60.0;
    static constexpr double kTimeToKeepRunnerAlive     = 3.0 * 60.0;

    // ── Shared state (all protected by stateMtx_) ────────────────────────────
    mutable std::mutex              stateMtx_;
    std::list<RunnerEntry>          lsOfRunner_;
    std::map<std::string, ApiSubscriber> lsOfApiSubscriber_;

    // ── KUKSA client ─────────────────────────────────────────────────────────
    std::unique_ptr<KuksaClient>    kuksa_;

    // ── Socket.IO client ─────────────────────────────────────────────────────
    sio::client                     sio_;
    std::atomic_bool                connected_{ false }; // true only while session is open

    // Used by the connect loop to block until close_listener fires
    std::mutex              sioCloseMtx_;
    std::condition_variable sioCloseCv_;
    bool                    sioSessionClosed_{ true };

    // ── Ticker threads ───────────────────────────────────────────────────────
    std::atomic_bool                running_{ false };
    std::thread                     tickerFastThread_;
    std::thread                     tickerThread_;
    std::thread                     ticker5sThread_;

    void tickerFast();    // 0.3 s – push signal values to subscribers
    void ticker();        // 1.0 s – evict stale entries
    void ticker5s();      // 5.0 s – report runtime state

    // ── Socket.IO helpers ────────────────────────────────────────────────────
    void onConnect();
    void onDisconnect();
    void onMessage(sio::event& ev);

    // Emit a JSON object as a Socket.IO message
    void emit(const std::string& event, const nlohmann::json& payload);

    // Build a sio::message from a nlohmann::json object
    static std::shared_ptr<sio::message> jsonToSio(const nlohmann::json& j);
    static nlohmann::json sioToJson(const std::shared_ptr<sio::message>& msg);

    // ── Command handlers ─────────────────────────────────────────────────────
    void handleDeployRequest      (const nlohmann::json& data);
    void handleSubscribeApis      (const nlohmann::json& data);
    void handleUnsubscribeApis    (const nlohmann::json& data);
    void handleListMockSignal     (const nlohmann::json& data);
    void handleSetMockSignals     (const nlohmann::json& data);
    void handleWriteSignalsValue  (const nlohmann::json& data);
    void handleResetSignalsValue  (const nlohmann::json& data);
    void handleGenerateVehicleModel(const nlohmann::json& data);
    void handleRevertVehicleModel  (const nlohmann::json& data);
    void handleListPythonPackages  (const nlohmann::json& data);
    void handleInstallPythonPackages(const nlohmann::json& data);
    void handleRunPythonApp        (const nlohmann::json& data);
    void handleRunBinApp           (const nlohmann::json& data);
    void handleStopPythonApp       (const nlohmann::json& data);
    void handleGetRuntimeInfo      (const nlohmann::json& data);

    // ── App run helpers ──────────────────────────────────────────────────────
    void sendAppRunReply   (const std::string& masterId, bool isDone,
                            int retcode, const std::string& content);
    void sendDeployReply   (const std::string& masterId, const std::string& content,
                            bool isFinish, const std::string& cmd = "deploy-request");
    bool installDependencies(const std::string& requestFrom);

    // ── Mock signal management ───────────────────────────────────────────────
    nlohmann::json listMockSignal() const;
    void           modifyMockSignal(const nlohmann::json& signalArray);
    void           appendMockSignal(const std::vector<std::string>& signals);
    void           writeSignalsValue(const nlohmann::json& signalValues);

    // ── Misc helpers ─────────────────────────────────────────────────────────
    nlohmann::json convertRunnersToJson() const;
    double         now() const;   // epoch seconds as double
    bool           isProcessRunning(const std::string& name) const;
};

} // namespace sdv
