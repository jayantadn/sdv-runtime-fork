// Copyright (c) 2025 Eclipse Foundation.
//
// SPDX-License-Identifier: MIT

#include "syncer.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <thread>

#include <sio_client.h>
#include <sio_message.h>

#include "utils.hpp"
#include "project_utils.hpp"
#include "vehicle_model_manager.hpp"

namespace sdv {

namespace fs = std::filesystem;
using json   = nlohmann::json;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

double Syncer::now() const {
    using namespace std::chrono;
    return duration_cast<duration<double>>(
        system_clock::now().time_since_epoch()).count();
}

bool Syncer::isProcessRunning(const std::string& name) const {
    return utils::isProcessRunning(name);
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

Syncer::Syncer() {
    brokerHost_     = "127.0.0.1";
    brokerPort_     = 55555;
    mockSignalPath_ = "/home/dev/ws/mock/signals.json";

    const char* serverEnv = ::getenv("SYNCER_SERVER_URL");
    serverUrl_ = serverEnv ? std::string(serverEnv)
                           : "https://kit.digitalauto.tech";

    const char* prefixEnv = ::getenv("RUNTIME_PREFIX");
    const char* nameEnv   = ::getenv("RUNTIME_NAME");
    std::string prefix = prefixEnv ? std::string(prefixEnv) : "Runtime-";
    std::string rname  = nameEnv   ? std::string(nameEnv)   : "MyRuntime";
    clientId_ = prefix + rname;

    kuksa_ = std::make_unique<KuksaClient>(brokerHost_, brokerPort_);
}

Syncer::~Syncer() {
    stop();
}

// ---------------------------------------------------------------------------
// Run / stop
// ---------------------------------------------------------------------------

void Syncer::run() {
    std::cout << "RunTime display name: " << clientId_ << std::flush << std::endl;

    kuksa_->connect();

    // Disable the library's internal auto-reconnect so our loop controls retry.
    sio_.set_reconnect_attempts(0);

    // Register Socket.IO event handlers.
    // IMPORTANT: sio_.socket()->on() must only be called after the session is
    // open (i.e. from inside the open listener), otherwise sio_ throws
    // "No active session".
    sio_.set_open_listener([this] {
        sio_.socket()->on("messageToKit", [this](sio::event& ev) { onMessage(ev); });
        onConnect();
    });
    sio_.set_close_listener([this](sio::client::close_reason) {
        {
            std::unique_lock<std::mutex> lk(sioCloseMtx_);
            sioSessionClosed_ = true;
        }
        sioCloseCv_.notify_all();
        onDisconnect();
    });

    running_.store(true);

    // Start ticker threads
    tickerFastThread_ = std::thread([this] { tickerFast(); });
    tickerThread_     = std::thread([this] { ticker(); });
    ticker5sThread_   = std::thread([this] { ticker5s(); });

    // Connect loop: reconnect whenever the Kit Server connection drops
    while (running_.load()) {
        std::cout << "Connecting to Kit Server: " << serverUrl_ << std::endl;
        {
            std::unique_lock<std::mutex> lk(sioCloseMtx_);
            sioSessionClosed_ = false;
        }
        sio_.connect(serverUrl_);

        // Block until close_listener fires (session ended) or stop() is called.
        // Do NOT call sync_close() here — it sends a close frame immediately.
        {
            std::unique_lock<std::mutex> lk(sioCloseMtx_);
            sioCloseCv_.wait(lk, [this] { return sioSessionClosed_ || !running_.load(); });
        }

        if (!running_.load()) break; // stop() was called

        connected_.store(false);
        std::cerr << "[Syncer] Connection lost or failed. Retrying in 5s..." << std::endl;
        // Interruptible sleep: wake early if stop() is called
        for (int i = 0; i < 50 && running_.load(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Signal threads to stop, then join them before touching sio_ further
    running_.store(false);
    connected_.store(false);
    if (tickerFastThread_.joinable()) tickerFastThread_.join();
    if (tickerThread_.joinable())     tickerThread_.join();
    if (ticker5sThread_.joinable())   ticker5sThread_.join();

    sio_.clear_con_listeners();
}

void Syncer::stop() {
    running_.store(false);
    connected_.store(false);
    sioCloseCv_.notify_all(); // wake the connect loop
    if (sio_.opened()) {
        sio_.close();
    }
}

// ---------------------------------------------------------------------------
// Socket.IO lifecycle
// ---------------------------------------------------------------------------

void Syncer::onConnect() {
    connected_.store(true);
    std::cout << "Connected to Kit Server " << serverUrl_ << std::flush << std::endl;
    emit("register_kit", {
        {"kit_id", clientId_},
        {"name",   clientId_}
    });
}

void Syncer::onDisconnect() {
    connected_.store(false);
    std::cout << "Disconnected from Kit Server." << std::flush << std::endl;
}

// ---------------------------------------------------------------------------
// Message routing
// ---------------------------------------------------------------------------

void Syncer::onMessage(sio::event& ev) {
    try {
        json data = sioToJson(ev.get_message());
        std::string cmd = data.value("cmd", "");

        if (cmd == "deploy_request" || cmd == "deploy-request") {
            handleDeployRequest(data);
        } else if (cmd == "subscribe_apis") {
            handleSubscribeApis(data);
        } else if (cmd == "unsubscribe_apis") {
            handleUnsubscribeApis(data);
        } else if (cmd == "list_mock_signal") {
            handleListMockSignal(data);
        } else if (cmd == "set_mock_signals") {
            handleSetMockSignals(data);
        } else if (cmd == "write_signals_value") {
            handleWriteSignalsValue(data);
        } else if (cmd == "reset_signals_value") {
            handleResetSignalsValue(data);
        } else if (cmd == "generate_vehicle_model") {
            handleGenerateVehicleModel(data);
        } else if (cmd == "revert_vehicle_model") {
            handleRevertVehicleModel(data);
        } else if (cmd == "list_python_packages") {
            handleListPythonPackages(data);
        } else if (cmd == "install_python_packages") {
            handleInstallPythonPackages(data);
        } else if (cmd == "run_python_app") {
            handleRunPythonApp(data);
        } else if (cmd == "run_bin_app") {
            handleRunBinApp(data);
        } else if (cmd == "stop_python_app") {
            handleStopPythonApp(data);
        } else if (cmd == "get-runtime-info") {
            handleGetRuntimeInfo(data);
        } else {
            std::cerr << "Unknown command: " << cmd << std::endl;
        }
    } catch (const std::exception& e) {
        std::cerr << "Error handling message: " << e.what() << std::endl;
    }
}

// ---------------------------------------------------------------------------
// Ticker threads
// ---------------------------------------------------------------------------

void Syncer::tickerFast() {
    while (running_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));

        std::map<std::string, ApiSubscriber> snapshot;
        {
            std::lock_guard<std::mutex> lk(stateMtx_);
            snapshot = lsOfApiSubscriber_;
        }

        if (snapshot.empty()) continue;

        if (!kuksa_->isConnected()) {
            try { kuksa_->connect(); } catch (const std::exception& e) {
                std::cerr << "[tickerFast] reconnect: " << e.what() << std::endl;
            } catch (...) {
                std::cerr << "[tickerFast] reconnect: unknown exception" << std::endl;
            }
            continue;
        }

        for (const auto& [clientId, sub] : snapshot) {
            if (sub.apis.empty()) continue;
            try {
                std::map<std::string, DatapointValue> values;
                for (const auto& api : sub.apis) {
                    auto result = kuksa_->getCurrentValues({ api });
                    values.insert(result.begin(), result.end());
                }

                json result = json::object();
                for (const auto& [path, val] : values) {
                    std::visit([&](auto&& v) {
                        using T = std::decay_t<decltype(v)>;
                        if constexpr (std::is_same_v<T, std::monostate>) {
                            result[path] = nullptr;
                        } else if constexpr (std::is_same_v<T, std::vector<bool>>   ||
                                             std::is_same_v<T, std::vector<int32_t>> ||
                                             std::is_same_v<T, std::vector<int64_t>> ||
                                             std::is_same_v<T, std::vector<uint32_t>>||
                                             std::is_same_v<T, std::vector<uint64_t>>||
                                             std::is_same_v<T, std::vector<float>>   ||
                                             std::is_same_v<T, std::vector<double>>  ||
                                             std::is_same_v<T, std::vector<std::string>>) {
                            json arr = json::array();
                            for (const auto& elem : v) arr.push_back(elem);
                            result[path] = arr;
                        } else {
                            result[path] = v;
                        }
                    }, val);
                }

                emit("messageToKit-kitReply", {
                    {"kit_id",      clientId_},
                    {"request_from", clientId},
                    {"cmd",         "apis-value"},
                    {"result",      result}
                });
            } catch (const std::exception& e) {
                std::cerr << "tickerFast error: " << e.what() << std::endl;
            }
        }
    }
}

void Syncer::ticker() {
    while (running_.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));

        double currentTime = now();
        std::lock_guard<std::mutex> lk(stateMtx_);

        // Evict stale API subscribers
        for (auto it = lsOfApiSubscriber_.begin();
             it != lsOfApiSubscriber_.end(); ) {
            if (currentTime - it->second.registeredAt >
                kTimeToKeepSubscriberAlive) {
                it = lsOfApiSubscriber_.erase(it);
            } else {
                ++it;
            }
        }

        // Evict and kill stale runners
        for (auto it = lsOfRunner_.begin(); it != lsOfRunner_.end(); ) {
            if (currentTime - it->startTime > kTimeToKeepRunnerAlive) {
                try { if (it->runner) it->runner->kill(); } catch (const std::exception& e) {
                    std::cerr << "[ticker] evict runner kill: " << e.what() << std::endl;
                } catch (...) {
                    std::cerr << "[ticker] evict runner kill: unknown exception" << std::endl;
                }
                it = lsOfRunner_.erase(it);
            } else {
                ++it;
            }
        }
    }
}

void Syncer::ticker5s() {
    std::string lastRunnersJson;
    size_t      lastSubscriberCount = 0;

    while (running_.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));

        json runnersJson;
        size_t subscriberCount;
        std::map<std::string, ApiSubscriber> subSnapshot;

        {
            std::lock_guard<std::mutex> lk(stateMtx_);
            runnersJson      = convertRunnersToJson();
            subscriberCount  = lsOfApiSubscriber_.size();
            subSnapshot      = lsOfApiSubscriber_;
        }

        if (subscriberCount == 0) continue;

        std::string runnersStr = runnersJson.dump();
        if (runnersStr == lastRunnersJson &&
            subscriberCount == lastSubscriberCount) continue;

        lastRunnersJson      = runnersStr;
        lastSubscriberCount  = subscriberCount;

        try {
            emit("report-runtime-state", {
                {"kit_id", clientId_},
                {"data", {
                    {"noOfRunner",        lsOfRunner_.size()},
                    {"noOfApiSubscriber", subscriberCount}
                }}
            });

            for (const auto& [clientSid, sub] : subSnapshot) {
                json safeSubscribers = json::object();
                for (const auto& [k, v] : subSnapshot) {
                    safeSubscribers[k] = {
                        {"apis",       v.apis},
                        {"keep_alive", 0}
                    };
                }
                emit("messageToKit-kitReply", {
                    {"kit_id",      clientId_},
                    {"request_from", clientSid},
                    {"cmd",         "report-runtime-state"},
                    {"data", {
                        {"lsOfRunner",        runnersJson},
                        {"lsOfApiSubscriber", safeSubscribers}
                    }}
                });
            }
        } catch (const std::exception& e) {
            std::cerr << "ticker5s error: " << e.what() << std::endl;
        }
    }
}

// ---------------------------------------------------------------------------
// Command handlers
// ---------------------------------------------------------------------------

void Syncer::handleDeployRequest(const json& data) {
    std::string requestFrom = data.value("request_from", "");
    sendDeployReply(requestFrom, "Receive deploy request \r\n", false,
                    data.value("cmd", "deploy-request"));
    std::this_thread::sleep_for(std::chrono::seconds(1));

    if (data.contains("code") && data["code"].is_string()) {
        utils::writeCodeToFile(data["code"].get<std::string>());
    }

    sendDeployReply(requestFrom, "Check syntax.... \r\n",  false, data.value("cmd","deploy-request"));
    std::this_thread::sleep_for(std::chrono::seconds(3));
    sendDeployReply(requestFrom, "Build docker image \r\n", false, data.value("cmd","deploy-request"));
    std::this_thread::sleep_for(std::chrono::seconds(3));
    sendDeployReply(requestFrom, "Send to HW kit \r\n",     false, data.value("cmd","deploy-request"));
    std::this_thread::sleep_for(std::chrono::seconds(3));
    sendDeployReply(requestFrom, "Run docker on HW kit \r\n",false, data.value("cmd","deploy-request"));
    std::this_thread::sleep_for(std::chrono::seconds(3));
    sendDeployReply(requestFrom, "Deploy done! \r\n",        true,  data.value("cmd","deploy-request"));
}

void Syncer::handleSubscribeApis(const json& data) {
    if (!data.contains("apis") || !data["apis"].is_array()) return;

    std::string requestFrom = data.value("request_from", "");
    std::vector<std::string> apis;
    for (const auto& a : data["apis"]) {
        if (a.is_string()) apis.push_back(a.get<std::string>());
    }

    {
        std::lock_guard<std::mutex> lk(stateMtx_);
        lsOfApiSubscriber_[requestFrom] = { now(), apis };
    }

    if (!apis.empty()) {
        try { appendMockSignal(apis); } catch (const std::exception& e) {
            std::cerr << "[handleSubscribeApis] appendMockSignal: " << e.what() << std::endl;
        } catch (...) {
            std::cerr << "[handleSubscribeApis] appendMockSignal: unknown exception" << std::endl;
        }
    }

    emit("messageToKit-kitReply", {
        {"kit_id",      clientId_},
        {"request_from", requestFrom},
        {"cmd",         "subscribe_apis"},
        {"result",      "Successful"}
    });
}

void Syncer::handleUnsubscribeApis(const json& data) {
    std::string requestFrom = data.value("request_from", "");
    {
        std::lock_guard<std::mutex> lk(stateMtx_);
        lsOfApiSubscriber_.erase(requestFrom);
    }
    emit("messageToKit-kitReply", {
        {"kit_id",      clientId_},
        {"request_from", requestFrom},
        {"cmd",         "unsubscribe_apis"},
        {"result",      "Successful"}
    });
}

void Syncer::handleListMockSignal(const json& data) {
    std::string requestFrom = data.value("request_from", "");
    emit("messageToKit-kitReply", {
        {"kit_id",      clientId_},
        {"request_from", requestFrom},
        {"cmd",         "list_mock_signal"},
        {"data",        listMockSignal()},
        {"result",      "Successful"}
    });
}

void Syncer::handleSetMockSignals(const json& data) {
    std::string requestFrom = data.value("request_from", "");
    if (data.contains("data") && data["data"].is_array()) modifyMockSignal(data["data"]);
    utils::restartMockProvider();
    emit("messageToKit-kitReply", {
        {"kit_id",      clientId_},
        {"request_from", requestFrom},
        {"cmd",         "set_mock_signals"},
        {"data",        listMockSignal()},
        {"result",      "Successful"}
    });
}

void Syncer::handleWriteSignalsValue(const json& data) {
    std::string requestFrom = data.value("request_from", "");
    if (data.contains("data") && data["data"].is_object()) writeSignalsValue(data["data"]);
    emit("messageToKit-kitReply", {
        {"kit_id",      clientId_},
        {"request_from", requestFrom},
        {"cmd",         "write_signals_value"},
        {"data",        json::object()},
        {"result",      "Successful"}
    });
}

void Syncer::handleResetSignalsValue(const json& data) {
    std::string requestFrom = data.value("request_from", "");
    std::string raw = utils::readMockSignalFile(mockSignalPath_);
    try {
        json parsed = json::parse(raw);
        if (parsed.is_object()) writeSignalsValue(parsed);
    } catch (const std::exception& e) {
        std::cerr << "[handleResetSignalsValue] parse signal file: " << e.what() << std::endl;
    }
    emit("messageToKit-kitReply", {
        {"kit_id",      clientId_},
        {"request_from", requestFrom},
        {"cmd",         "reset_signals_value"},
        {"data",        listMockSignal()},
        {"result",      "Successful"}
    });
}

void Syncer::handleGenerateVehicleModel(const json& data) {
    std::string requestFrom = data.value("request_from", "");
    std::cout << "receive request generate_vehicle_model" << std::endl;

    emit("messageToKit-kitReply", {
        {"kit_id",      clientId_},
        {"request_from", requestFrom},
        {"cmd",         "revert_vehicle_model"},
        {"result",      "Start to rebuild vehicle model...\r\n"}
    });

    try {
        utils::stopMockService();

        if (!data.contains("data") || !data["data"].is_object()) {
            throw std::runtime_error("generate_vehicle_model: 'data' field must be a JSON object");
        }
        std::string inputJson = data["data"].dump();
        vehicle_model::generateVehicleModel(inputJson);

        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        const char* disableDB = ::getenv("DISABLE_DATABROKER");
        if (!disableDB || std::string(disableDB).empty()) {
            if (isProcessRunning("databroker")) {
                std::cout << "databroker is running" << std::endl;
            } else {
                throw std::runtime_error("Databroker is not running");
            }
            kuksa_->waitUntilReady();
        }

        modifyMockSignal(json::array({ "" }));
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        utils::startMockService();

        emit("messageToKit-kitReply", {
            {"kit_id",      clientId_},
            {"request_from", requestFrom},
            {"cmd",         "generate_vehicle_model"},
            {"result",      "Generate new model Successful"}
        });
    } catch (const std::exception& e) {
        emit("messageToKit-kitReply", {
            {"kit_id",      clientId_},
            {"request_from", requestFrom},
            {"cmd",         "generate_vehicle_model"},
            {"result",      "Error: generate_vehicle_model Failed: " +
                             std::string(e.what()) +
                             "\r\nRevert back to default model"}
        });
        try { vehicle_model::revertVehicleModel(); } catch (const std::exception& re) {
            std::cerr << "[handleGenerateVehicleModel] revert also failed: " << re.what() << std::endl;
        } catch (...) {
            std::cerr << "[handleGenerateVehicleModel] revert also failed: unknown exception" << std::endl;
        }
    }
}

void Syncer::handleRevertVehicleModel(const json& data) {
    std::string requestFrom = data.value("request_from", "");
    emit("messageToKit-kitReply", {
        {"kit_id",      clientId_},
        {"request_from", requestFrom},
        {"cmd",         "revert_vehicle_model"},
        {"result",      "Start to revert to default vehicle model...\r\n"}
    });

    utils::stopMockService();
    vehicle_model::revertVehicleModel();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    utils::startMockService();

    emit("messageToKit-kitReply", {
        {"kit_id",      clientId_},
        {"request_from", requestFrom},
        {"cmd",         "revert_vehicle_model"},
        {"result",      "Revert to default Vehicle Model Successful\r\n"}
    });
}

void Syncer::handleListPythonPackages(const json& data) {
    std::string requestFrom = data.value("request_from", "");

    // Invoke pip freeze as subprocess
    std::string pkgs;
    FILE* f = ::popen("pip freeze", "r");
    if (f) {
        char buf[256];
        while (::fgets(buf, sizeof(buf), f)) pkgs += buf;
        ::pclose(f);
    }

    emit("messageToKit-kitReply", {
        {"kit_id",      clientId_},
        {"request_from", requestFrom},
        {"cmd",         "list_python_packages"},
        {"data",        pkgs},
        {"result",      "Successful"}
    });
}

void Syncer::handleInstallPythonPackages(const json& data) {
    std::string requestFrom = data.value("request_from", "");
    std::string pkgStr;
    if (data.contains("data") && data["data"].is_string())
        pkgStr = data["data"].get<std::string>();

    if (pkgStr.empty()) {
        emit("messageToKit-kitReply", {
            {"kit_id",      clientId_},
            {"request_from", requestFrom},
            {"cmd",         "install_python_packages"},
            {"result",      "Error: missing package name"},
            {"data",        ""}
        });
        return;
    }

    emit("messageToKit-kitReply", {
        {"kit_id",      clientId_},
        {"request_from", requestFrom},
        {"cmd",         "install_python_packages"},
        {"result",      "Installing"},
        {"data",        "Installing packages: " + pkgStr + "\n"}
    });

    // Get username for target directory
    const char* user = ::getenv("USER");
    if (!user) user = ::getenv("USERNAME");
    std::string userStr = user ? user : "dev";

    std::string cmd = "pip install --target /home/" + userStr +
                      "/python-packages " + pkgStr + " 2>&1";
    std::string response;
    FILE* f = ::popen(cmd.c_str(), "r");
    if (f) {
        char buf[256];
        while (::fgets(buf, sizeof(buf), f)) response += buf;
        ::pclose(f);
    }

    emit("messageToKit-kitReply", {
        {"kit_id",      clientId_},
        {"request_from", requestFrom},
        {"cmd",         "install_python_packages"},
        {"result",      "Successful"},
        {"data",        response}
    });
}

void Syncer::handleRunPythonApp(const json& data) {
    std::string requestFrom = data.value("request_from", "");

    if (!data.contains("data") || !data["data"].is_object() ||
        !data["data"].contains("code") || !data["data"]["code"].is_string()) {
        emit("messageToKit-kitReply", {
            {"kit_id",      clientId_},
            {"request_from", requestFrom},
            {"cmd",         "run_python_app"},
            {"result",      "Error: Missing or invalid code field"},
            {"data",        ""}
        });
        return;
    }

    std::string appName  = data["data"].value("name", "App name");
    std::string codeData = data["data"]["code"].get<std::string>();
    std::string cmdToRun = "python3 -u main.py";
    bool        isProject = false;

    try {
        if (!json::accept(codeData)) {
            // Not valid JSON → treat as raw Python source
            throw json::parse_error::create(0, 0, "not JSON", nullptr);
        }
        isProject = true;
        project_utils::createProjectFromJson(codeData, "app");
        cmdToRun = "python3 -u app/main.py";
    } catch (const json::parse_error&) {
        utils::writeCodeToFile(codeData);
    }

    if (isProject) {
        if (!installDependencies(requestFrom)) return;
    }

    // Append mock signals for used APIs
    if (data.contains("usedAPIs") && data["usedAPIs"].is_array()) {
        std::vector<std::string> usedApis;
        for (const auto& a : data["usedAPIs"]) {
            if (a.is_string()) usedApis.push_back(a.get<std::string>());
        }
        if (!usedApis.empty()) {
            try { appendMockSignal(usedApis); } catch (const std::exception& e) {
                std::cerr << "[handleRunPythonApp] appendMockSignal: " << e.what() << std::endl;
            } catch (...) {
                std::cerr << "[handleRunPythonApp] appendMockSignal: unknown exception" << std::endl;
            }
        }
    }

    auto stdoutCb  = [this, requestFrom](const std::string&, const std::string& line) {
        sendAppRunReply(requestFrom, false, 0, line + "\r\n");
    };
    auto stderrCb  = stdoutCb;
    auto finishedCb = [this, requestFrom](const std::string&, int retcode) {
        sendAppRunReply(requestFrom, true, retcode, "");
    };

    auto proc = std::make_shared<SubPiper>(cmdToRun, requestFrom,
                                           stdoutCb, stderrCb, finishedCb);
    proc->start();

    std::lock_guard<std::mutex> lk(stateMtx_);
    lsOfRunner_.push_back({ appName, proc, requestFrom, now() });
}

void Syncer::handleRunBinApp(const json& data) {
    std::string requestFrom = data.value("request_from", "");
    std::string appName;
    if (data.contains("data") && data["data"].is_string())
        appName = data["data"].get<std::string>();
    if (appName.empty()) {
        emit("messageToKit-kitReply", {
            {"kit_id",      clientId_},
            {"request_from", requestFrom},
            {"cmd",         "run_bin_app"},
            {"result",      "Error: missing app name"},
            {"data",        ""}
        });
        return;
    }
    std::string appPath = "/home/dev/output/" + appName;

    if (!fs::exists(appPath)) {
        emit("messageToKit-kitReply", {
            {"kit_id",      clientId_},
            {"request_from", requestFrom},
            {"cmd",         "run_bin_app"},
            {"result",      "Failed: app not found"},
            {"data",        ""}
        });
        return;
    }

    if (data.contains("usedAPIs") && data["usedAPIs"].is_array()) {
        std::vector<std::string> usedApis;
        for (const auto& a : data["usedAPIs"]) {
            if (a.is_string()) usedApis.push_back(a.get<std::string>());
        }
        try { appendMockSignal(usedApis); } catch (const std::exception& e) {
            std::cerr << "[handleRunBinApp] appendMockSignal: " << e.what() << std::endl;
        } catch (...) {
            std::cerr << "[handleRunBinApp] appendMockSignal: unknown exception" << std::endl;
        }
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    auto stdoutCb   = [this, requestFrom](const std::string&, const std::string& line) {
        sendAppRunReply(requestFrom, false, 0, line + "\r\n");
    };
    auto stderrCb   = stdoutCb;
    auto finishedCb = [this, requestFrom](const std::string&, int retcode) {
        sendAppRunReply(requestFrom, true, retcode, "");
    };

    auto proc = std::make_shared<SubPiper>(appPath, requestFrom,
                                           stdoutCb, stderrCb, finishedCb);
    proc->start();

    std::lock_guard<std::mutex> lk(stateMtx_);
    lsOfRunner_.push_back({ appName, proc, requestFrom, now() });
}

void Syncer::handleStopPythonApp(const json& data) {
    std::string requestFrom = data.value("request_from", "");
    std::lock_guard<std::mutex> lk(stateMtx_);
    for (auto it = lsOfRunner_.begin(); it != lsOfRunner_.end(); ) {
        if (it->requestFrom == requestFrom) {
            try { if (it->runner) it->runner->kill(); } catch (const std::exception& e) {
                std::cerr << "[handleStopPythonApp] runner kill: " << e.what() << std::endl;
            } catch (...) {
                std::cerr << "[handleStopPythonApp] runner kill: unknown exception" << std::endl;
            }
            it = lsOfRunner_.erase(it);
        } else {
            ++it;
        }
    }
}

void Syncer::handleGetRuntimeInfo(const json& data) {
    std::string requestFrom = data.value("request_from", "");

    json safeSubscribers = json::object();
    json runnersJson;
    {
        std::lock_guard<std::mutex> lk(stateMtx_);
        runnersJson = convertRunnersToJson();
        for (const auto& [k, v] : lsOfApiSubscriber_) {
            safeSubscribers[k] = {
                {"apis",       v.apis},
                {"keep_alive", 0}
            };
        }
    }

    emit("messageToKit-kitReply", {
        {"kit_id",      clientId_},
        {"request_from", requestFrom},
        {"cmd",         "get-runtime-info"},
        {"data", {
            {"lsOfRunner",        runnersJson},
            {"lsOfApiSubscriber", safeSubscribers}
        }}
    });
}

// ---------------------------------------------------------------------------
// App run helpers
// ---------------------------------------------------------------------------

void Syncer::sendAppRunReply(const std::string& masterId, bool isDone,
                             int retcode, const std::string& content) {
    emit("messageToKit-kitReply", {
        {"kit_id",      clientId_},
        {"request_from", masterId},
        {"cmd",         "run_python_app"},
        {"data",        ""},
        {"isDone",      isDone},
        {"result",      content},
        {"code",        retcode}
    });
}

void Syncer::sendDeployReply(const std::string& masterId,
                              const std::string& content,
                              bool isFinish,
                              const std::string& cmd) {
    emit("messageToKit-kitReply", {
        {"token",        "12a-124-45634-12345-1swer"},
        {"request_from", masterId},
        {"cmd",          cmd},
        {"data",         ""},
        {"result",       content},
        {"is_finish",    isFinish}
    });
}

bool Syncer::installDependencies(const std::string& requestFrom) {
    static const std::string kReqPath = "app/requirements.txt";
    if (!fs::exists(kReqPath)) return true;

    sendAppRunReply(requestFrom, false, 0,
                    "Installing dependencies from requirements.txt...\r\n");

    std::string cmd = "pip install -r " + kReqPath + " 2>&1";
    int ret = ::system(cmd.c_str());

    if (ret == -1 || !WIFEXITED(ret) || WEXITSTATUS(ret) != 0) {
        int exitCode = (ret != -1 && WIFEXITED(ret)) ? WEXITSTATUS(ret) : -1;
        sendAppRunReply(requestFrom, false, exitCode,
                        "Failed to install dependencies.\r\n");
        return false;
    }
    sendAppRunReply(requestFrom, false, 0,
                    "Dependencies installed successfully.\r\n");
    return true;
}

// ---------------------------------------------------------------------------
// Mock signal management
// ---------------------------------------------------------------------------

json Syncer::listMockSignal() const {
    std::string raw = utils::readMockSignalFile(mockSignalPath_);
    try {
        return json::parse(raw);
    } catch (const std::exception& e) {
        std::cerr << "[listMockSignal] parse signal file: " << e.what() << std::endl;
        return json::array();
    } catch (...) {
        std::cerr << "[listMockSignal] parse signal file: unknown exception" << std::endl;
        return json::array();
    }
}

void Syncer::modifyMockSignal(const json& signalArray) {
    if (!signalArray.is_array()) return;

    json finalSignals = json::array();
    for (const auto& sig : signalArray) {
        if (!sig.contains("signal")) continue;
        std::string path = sig["signal"].get<std::string>();
        try {
            auto meta = kuksa_->getMetadata({ path });
            if (!meta.empty()) {
                finalSignals.push_back(sig);
            }
        } catch (const std::exception& e) {
            std::cerr << "[modifyMockSignal] getMetadata for '" << path
                      << "': " << e.what() << std::endl;
        } catch (...) {
            std::cerr << "[modifyMockSignal] getMetadata for '" << path
                      << "': unknown exception" << std::endl;
        }
    }

    utils::writeMockSignalFile(mockSignalPath_, finalSignals.dump(4));
}

void Syncer::appendMockSignal(const std::vector<std::string>& signals) {
    if (signals.empty()) return;

    json curMocks;
    try {
        curMocks = json::parse(utils::readMockSignalFile(mockSignalPath_));
    } catch (const std::exception& e) {
        std::cerr << "[appendMockSignal] parse signal file: " << e.what() << std::endl;
        curMocks = json::array();
    } catch (...) {
        std::cerr << "[appendMockSignal] parse signal file: unknown exception" << std::endl;
        curMocks = json::array();
    }

    std::vector<std::string> curNames;
    for (const auto& m : curMocks) {
        if (m.contains("signal")) curNames.push_back(m["signal"].get<std::string>());
    }

    bool hasNew = false;
    for (const auto& sig : signals) {
        if (std::find(curNames.begin(), curNames.end(), sig) != curNames.end())
            continue;
        try {
            auto meta = kuksa_->getMetadata({ sig });
            if (!meta.empty()) {
                hasNew = true;
                curNames.push_back(sig);
                curMocks.push_back({ {"signal", sig}, {"value", "0"} });
                std::cout << ">>> Append new mock signal " << sig << std::endl;
            }
        } catch (const std::exception& e) {
            std::cerr << "[appendMockSignal] getMetadata for '" << sig
                      << "': " << e.what() << std::endl;
        } catch (...) {
            std::cerr << "[appendMockSignal] getMetadata for '" << sig
                      << "': unknown exception" << std::endl;
        }
    }

    if (hasNew) {
        utils::writeMockSignalFile(mockSignalPath_, curMocks.dump(4));
        utils::restartMockProvider();
    }
}

void Syncer::writeSignalsValue(const json& signalValues) {
    if (!signalValues.is_object()) return;

    for (const auto& [path, jval] : signalValues.items()) {
        try {
            auto meta = kuksa_->getMetadata({ path });
            if (meta.empty()) continue;

            EntryType et = meta.at(path).entryType;

            // Convert JSON value to DatapointValue (best-effort)
            DatapointValue dpv;
            if (jval.is_boolean())  dpv = jval.get<bool>();
            else if (jval.is_number_integer()) dpv = jval.get<int64_t>();
            else if (jval.is_number_float())   dpv = jval.get<double>();
            else if (jval.is_string())         dpv = jval.get<std::string>();
            else                               continue;

            if (et == EntryType::ACTUATOR) {
                kuksa_->setTargetValues({ { path, dpv } });
            } else if (et == EntryType::SENSOR) {
                kuksa_->setCurrentValues({ { path, dpv } });
            }
        } catch (const std::exception& e) {
            std::cerr << "writeSignalsValue error for " << path
                      << ": " << e.what() << std::endl;
        }
    }
}

// ---------------------------------------------------------------------------
// Misc helpers
// ---------------------------------------------------------------------------

json Syncer::convertRunnersToJson() const {
    json arr = json::array();
    for (const auto& r : lsOfRunner_) {
        arr.push_back({
            {"appName",      r.appName},
            {"request_from", r.requestFrom},
            {"from",         r.startTime}
        });
    }
    return arr;
}

// ---------------------------------------------------------------------------
// Socket.IO message conversion
// ---------------------------------------------------------------------------

void Syncer::emit(const std::string& event, const json& payload) {
    if (!connected_.load()) return;
    try {
        sio_.socket()->emit(event, jsonToSio(payload));
    } catch (const std::exception& e) {
        std::cerr << "[Syncer] emit '" << event << "' failed: " << e.what() << std::endl;
    }
}

std::shared_ptr<sio::message> Syncer::jsonToSio(const json& j) {
    if (j.is_null())    return sio::null_message::create();
    if (j.is_boolean()) return sio::bool_message::create(j.get<bool>());
    if (j.is_number_integer()) return sio::int_message::create(j.get<int64_t>());
    if (j.is_number_float())   return sio::double_message::create(j.get<double>());
    if (j.is_string())  return sio::string_message::create(j.get<std::string>());

    if (j.is_array()) {
        auto arr = sio::array_message::create();
        for (const auto& elem : j) {
            arr->get_vector().push_back(jsonToSio(elem));
        }
        return arr;
    }

    if (j.is_object()) {
        auto obj = sio::object_message::create();
        for (const auto& [k, v] : j.items()) {
            obj->get_map()[k] = jsonToSio(v);
        }
        return obj;
    }

    return sio::null_message::create();
}

json Syncer::sioToJson(const std::shared_ptr<sio::message>& msg) {
    if (!msg) return nullptr;
    switch (msg->get_flag()) {
    case sio::message::flag_null:
        return nullptr;
    case sio::message::flag_boolean:
        return msg->get_bool();
    case sio::message::flag_integer:
        return msg->get_int();
    case sio::message::flag_double:
        return msg->get_double();
    case sio::message::flag_string:
        return msg->get_string();
    case sio::message::flag_binary:
        return nullptr;   // binary not used
    case sio::message::flag_array: {
        json arr = json::array();
        for (const auto& elem : msg->get_vector()) {
            arr.push_back(sioToJson(elem));
        }
        return arr;
    }
    case sio::message::flag_object: {
        json obj = json::object();
        for (const auto& [k, v] : msg->get_map()) {
            obj[k] = sioToJson(v);
        }
        return obj;
    }
    default:
        return nullptr;
    }
}

} // namespace sdv
