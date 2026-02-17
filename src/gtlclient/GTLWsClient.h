#pragma once

#include <atomic>
#include <string>
#include <functional>
#include <thread>
#include <mutex>
#include <deque>
#include <chrono>
#include <unordered_set>

#include <ixwebsocket/IXWebSocket.h>
#include <nlohmann/json.hpp>

class GTLWsClient {
public:
    using json = nlohmann::json;

    using ProcessCommandFn = std::function<int(const json&)>; // processCommand(j, tid)
    using OkToProcessFn = std::function<int()>;                 // okToProcessCommands() (1/0)
    using GetGameCodeFn = std::function<std::string()>;         // getGameCode()

    GTLWsClient(ProcessCommandFn processCmd,
        OkToProcessFn okToProcess,
        GetGameCodeFn getGameCode)
        : processCmd_(std::move(processCmd))
        , okToProcess_(std::move(okToProcess))
        , getGameCode_(std::move(getGameCode)) {
    }

    ~GTLWsClient() { stop(); }

    void start(const std::string& url = "ws://127.0.0.1:14752");
    void stop();

    // Called by game code after it executes a command
    struct PlayedAck
    {
        std::string id;
        int reason; // 1=timeout, 2=death_fade
    };

    void notifyPlayed(const std::string& commandId, int reason = 0);

    static int64_t nowMs();

private:
    int  handleIncomingText(const std::string& s);
    void pumpLoop();

    

    std::mutex removedMu_;
    std::deque<std::pair<std::string, int>> removedQ_;

    ix::WebSocket     ws_;
    std::thread       pumpThread_;
    std::atomic<bool> running_{ false };

    // callbacks to your game code
    ProcessCommandFn  processCmd_;
    OkToProcessFn     okToProcess_;
    GetGameCodeFn     getGameCode_;

    // protocol state
    std::atomic<bool> isCommandAvailable_{ true };

    // played ack queue (thread-safe)
    std::deque<PlayedAck> playedQ_;
    std::mutex playedMu_;

    // pending commands (thread-safe) so we can "wait" without blocking ws callback
    std::mutex pendingMu_;
    std::deque<json> pendingQ_;

    // Dedup commands by id (thread-safe). Prevents duplicate execution after reconnect.
    std::mutex seenMu_;
    std::unordered_set<std::string> seenIds_;
    std::deque<std::string> seenOrder_;
    static constexpr size_t kMaxSeenIds = 5000;

    // helper: returns true if id was new and is now marked "seen"
    bool markSeenIfNew_(const std::string& id);

    // reconnect support
    std::string url_;
    std::atomic<bool> reconnectRequested_{ false };
    std::atomic<bool> reconnecting_{ false };
    std::atomic<int64_t> reconnectAtMs_{ 0 };
    static constexpr int64_t kReconnectDelayMs = 2000;

    // ping timing (atomic ms counter to avoid data races)
    std::atomic<int64_t> lastPingMs_{ 0 };

    // command ask watchdog: prevents "stuck false forever"
    std::atomic<int64_t> lastAskMs_{ 0 };
    static constexpr int64_t kAskTimeoutMs = 3000; // 3 seconds

};
