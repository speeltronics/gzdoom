#include "GTLWsClient.h"
#include <ixwebsocket/IXNetSystem.h>
#include <iostream>

using json = GTLWsClient::json;

int64_t GTLWsClient::nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

bool GTLWsClient::markSeenIfNew_(const std::string& id) {
    if (id.empty()) return true; // can't dedupe without an id

    std::lock_guard<std::mutex> lk(seenMu_);

    // already seen -> duplicate
    if (seenIds_.find(id) != seenIds_.end())
        return false;

    // mark as seen
    seenIds_.insert(id);
    seenOrder_.push_back(id);

    // cap memory: evict oldest ids
    while (seenOrder_.size() > kMaxSeenIds)
    {
        const std::string& old = seenOrder_.front();
        seenIds_.erase(old);
        seenOrder_.pop_front();
    }

    return true;
}

void GTLWsClient::start(const std::string& url) {
    if (running_.exchange(true)) return;

    url_ = url;
    lastPingMs_.store(0);
    lastAskMs_.store(0);
    reconnectRequested_.store(false);
    reconnecting_.store(false);
    reconnectAtMs_.store(0);

    ix::initNetSystem();

    ws_.setUrl(url_);

    ws_.setOnMessageCallback([this](const ix::WebSocketMessagePtr& msg)
        {
            using T = ix::WebSocketMessageType;

            if (msg->type == T::Open)
            {
                std::cout << "[ws] connected\n";
                lastPingMs_.store(0);
                lastAskMs_.store(0);
                isCommandAvailable_.store(true);

                // immediate ping so server can respond with cmdready
                const std::string gameCode = getGameCode_ ? getGameCode_() : "";
                json ping = { {"type","ping"}, {"romHash",gameCode}, {"gameCode",gameCode} };
                ws_.send(ping.dump());
                std::cout << "[ws->] ping (immediate)\n";
                return;
            }

            if (msg->type == T::Message)
            {
                // Keep this callback FAST. No waiting. No game calls.
                // std::cout << "[ws<-] " << msg->str << "\n";
                handleIncomingText(msg->str);
                return;
            }

            if (msg->type == T::Close)
            {
                std::cout << "[ws] disconnected\n";
                reconnectRequested_.store(true);
                reconnectAtMs_.store(nowMs() + kReconnectDelayMs);
                return;
            }

            if (msg->type == T::Error)
            {
                std::cout << "[ws] error: " << msg->errorInfo.reason << "\n";
                reconnectRequested_.store(true);
                reconnectAtMs_.store(nowMs() + kReconnectDelayMs);
                return;
            }
        });

    ws_.start();

    pumpThread_ = std::thread([this] { pumpLoop(); });
}

void GTLWsClient::stop() {
    if (!running_.exchange(false)) return;

    reconnectRequested_.store(false);

    try { ws_.stop(); }
    catch (...) {}

    if (pumpThread_.joinable()) pumpThread_.join();

    ix::uninitNetSystem();
}

void GTLWsClient::notifyPlayed(const std::string& commandId, int reason) {
    std::lock_guard<std::mutex> lk(playedMu_);
    playedQ_.push_back({ commandId, reason });
}

int GTLWsClient::handleIncomingText(const std::string& s) {
    if (s.empty()) return -1;

    // Legacy tokens
    if (s == "pong") return 0;
    if (s == "cmdready")
    {
        lastAskMs_.store(0);
        // only re-arm if we aren't holding pending commands
        std::lock_guard<std::mutex> lk(pendingMu_);
        if (pendingQ_.empty()) isCommandAvailable_.store(true);
        return 0;
    }

    try
    {
        auto j = json::parse(s);

        const std::string t = j.value("type", "");

        if (t == "pong") return 0;
        if (t == "cmdready")
        {
            lastAskMs_.store(0);
            std::lock_guard<std::mutex> lk(pendingMu_);
            if (pendingQ_.empty()) isCommandAvailable_.store(true);
            return 0;
        }

        // Detect command payloads (server sends full command objects without a "type")
        const bool looksLikeCommand =
            j.contains("id") && j.contains("command") && j.contains("nickname") && j.contains("health") &&
            j.contains("oIdx") && j.contains("aTimer") && j.contains("flFriendly");

        if (looksLikeCommand)
        {
            lastAskMs_.store(0);

            // Extract id safely (you said it's always a string UUID)
            std::string id;
            if (j.contains("id") && j["id"].is_string())
                id = j["id"].get<std::string>();

            // STRICT: refuse commands without a valid id (so we can dedupe safely)
            if (id.empty())
            {
                std::cout << "[ws] command missing string id, ignoring\n";
                isCommandAvailable_.store(true);  // re-arm asking
                return 0;
            }

            // Dedupe: if we've already accepted this id, ignore it
            if (!markSeenIfNew_(id))
            {
                return 0;
            }

            // Queue it and return immediately. Do NOT execute here.
            {
                std::lock_guard<std::mutex> lk(pendingMu_);
                pendingQ_.push_back(j);
            }

            isCommandAvailable_.store(false);
            return 0;
        }

        // Unknown/other JSON: don't deadlock the state machine
        isCommandAvailable_.store(true);
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cout << "[ws] parse/handle error: " << e.what() << "\n";
        isCommandAvailable_.store(true);
        return -1;
    }
}

void GTLWsClient::pumpLoop() {
    while (running_)
    {
        // Reconnect if requested and time reached
        if (reconnectRequested_.load())
        {
            const int64_t now = nowMs();

            // Only one reconnect attempt at a time
            if (now >= reconnectAtMs_.load() && !reconnecting_.exchange(true))
            {
                std::cout << "[ws] reconnecting...\n";

                try { ws_.stop(); }
                catch (...) {}

                bool startedOk = true;
                try
                {
                    ws_.setUrl(url_);
                    ws_.start();
                }
                catch (...)
                {
                    startedOk = false;
                }

                if (startedOk)
                {
                    reconnectRequested_.store(false);
                    reconnectAtMs_.store(0);
                    lastPingMs_.store(0);
                    lastAskMs_.store(0);
                    isCommandAvailable_.store(true);
                }
                else
                {
                    // try again later
                    reconnectRequested_.store(true);
                    reconnectAtMs_.store(nowMs() + kReconnectDelayMs);
                }

                reconnecting_.store(false); // ALWAYS release the guard
            }
        }

        if (ws_.getReadyState() == ix::ReadyState::Open)
        {
            const int64_t now = nowMs();

            // 0) If we have a pending command and the game is safe, execute ONE now
            json cmd;
            bool doRun = false;

            bool ok = !okToProcess_ || okToProcess_() != 0;
            if (ok)
            {
                std::lock_guard<std::mutex> lk(pendingMu_);
                if (!pendingQ_.empty())
                {
                    cmd = pendingQ_.front();
                    pendingQ_.pop_front();
                    doRun = true;
                }
            }

            if (doRun && processCmd_)
            {
                int rc = processCmd_(cmd);

                if (rc == 1)
                {
                    // DEFER: game wasnt safe. Put it back at the front.
                    std::lock_guard<std::mutex> lk(pendingMu_);
                    pendingQ_.push_front(cmd);
                    isCommandAvailable_.store(false);
                }
                else if (rc < 0)
                {
                    cmd["_tries"] = cmd.value("_tries", 0) + 1;
                    if (cmd["_tries"].get<int>() > 10)
                    {
                        std::cout << "[ws] dropping command after 10 failures id=" << cmd.value("id", "") << "\n";
                        // optionally: send a failure ack to server here if you support it
                    }
                    else
                    {
                        std::lock_guard<std::mutex> lk(pendingMu_);
                        pendingQ_.push_back(cmd);
                    }
                    isCommandAvailable_.store(false);
                }
                else
                {
                    // SUCCESS
                    std::lock_guard<std::mutex> lk(pendingMu_);
                    if (pendingQ_.empty()) isCommandAvailable_.store(true);
                }
            }

            // 1) Ping every ~2 seconds
            const int64_t lastPing = lastPingMs_.load();
            if (lastPing == 0 || (now - lastPing) >= 2000)
            {
                const std::string gameCode = getGameCode_ ? getGameCode_() : "";
                json ping = { {"type","ping"}, {"romHash",gameCode}, {"gameCode",gameCode} };
                ws_.send(ping.dump());
                lastPingMs_.store(now);
            }

            // 2) Send played acks (drain queue)
            for (;;)
            {
                PlayedAck ack;
                {
                    std::lock_guard<std::mutex> lk(playedMu_);
                    if (playedQ_.empty()) break;
                    ack = playedQ_.front();
                    playedQ_.pop_front();
                }

                const std::string gameCode = getGameCode_ ? getGameCode_() : "";
                json played = {
                    {"type","played"},
                    {"id", ack.id},
                    {"reason", ack.reason},          // <-- add this
                    {"romHash", gameCode},
                    {"gameCode", gameCode}
                };
                ws_.send(played.dump());
            }

            // 2.5) Ask watchdog: if we asked and nothing came back, re-arm asking
            // Only applies when we're currently "waiting" (isCommandAvailable_ == false)
            // and we have no pending commands queued.
            if (!isCommandAvailable_.load())
            {
                bool hasPending = false;
                {
                    std::lock_guard<std::mutex> lk(pendingMu_);
                    hasPending = !pendingQ_.empty();
                }

                if (!hasPending)
                {
                    const int64_t lastAsk = lastAskMs_.load();
                    if (lastAsk != 0 && (now - lastAsk) > kAskTimeoutMs)
                    {
                        std::cout << "[ws] ask timeout -> re-arming\n";
                        isCommandAvailable_.store(true);
                        lastAskMs_.store(0);
                    }
                }
            }

            // 3) Ask for a command when available and game is ready and no pending cmds
            if (isCommandAvailable_.load())
            {
                bool ok = !okToProcess_ || okToProcess_() != 0;
                if (ok)
                {
                    // don't ask if we already have pending queued commands
                    {
                        std::lock_guard<std::mutex> lk(pendingMu_);
                        if (!pendingQ_.empty())
                        {
                            isCommandAvailable_.store(false);
                            goto sleep_label;
                        }
                    }

                    const std::string gameCode = getGameCode_ ? getGameCode_() : "";
                    json ask = {
                        {"type","command"},
                        {"cmdTypesAvailable", {"actor","item"}}, // include both
                        {"romHash",gameCode},
                        {"gameCode",gameCode}
                    };
                    ws_.send(ask.dump());
                    lastAskMs_.store(now);
                    isCommandAvailable_.store(false);
                }
            }
        }

    sleep_label:
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
}