#if defined(_WIN32)
#include "i_mainwindow.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <vector>
#include <sstream>
#include <ctype.h>

#include "gi.h"
#include "p_acs.h"
#include "p_lnspec.h"
#include "sbar.h"
#include "d_player.h"
#include "g_game.h"
#include "g_level.h"
#include "p_local.h"
#include "p_effect.h"
#include "c_console.h"
#include "b_bot.h"
#include "doomstat.h"
#include "sbar.h"
#include "r_data/r_interpolate.h"
#include "d_player.h"
#include "r_utility.h"
#include "p_spec.h"
#include "g_levellocals.h"
#include "events.h"
#include "actorinlines.h"
#include "g_game.h"
#include "i_interface.h"
#include "vectors.h"
#include "c_cvars.h"
#include "doomstat.h"
#include "info.h"
#include "c_dispatch.h"
#include "d_net.h"
#include "v_text.h"
#endif

#include <chrono>
#include <thread>
#include <iostream>
#include <string>

#include "x86.h"
#include "version.h"
#include "v_video.h"
#include "gl_interface.h"
#include "printf.h"
#include "gamestate.h"

// ---- GTL WebSocket client wrapper ----------------------------------------
#include "gtlclient/GTLWsClient.h"

#include "c_cvars.h"

// ACS -> C++ event mailbox
CVAR(Int, gtl_evt_seq, 0, CVAR_MOD);
CVAR(Int, gtl_evt_tid, 0, CVAR_MOD);
CVAR(Int, gtl_evt_reason, 0, CVAR_MOD); // 1=timeout, 2=death_fade
CVAR(Int, gtl_suppress_actor_hud, 0, CVAR_MOD);
//CUSTOM_CVAR(Int, gtl_evt_seq, 0, 0);
//CUSTOM_CVAR(Int, gtl_evt_tid, 0, 0);
//CUSTOM_CVAR(Int, gtl_evt_reason, 0, 0);

#include <unordered_map>
#include <mutex>

struct GTLActiveCmd
{
    std::string id;
    int tid = 0;

    std::string nickname;
    int objectIdx = 0;
    int flFriendly = 0;

    int kindHealthParam = 0;   // >0 = monster (show health, fade on death), 0 = item
    int aTimer = 0;            // original timer seconds
    int64_t expireMs = 0;      // nowMs() + aTimer*1000 if aTimer>0

    // used only for respawn
    int respawnTimer = 0;      // remaining seconds at wipe time
};

static std::mutex g_tidMapMu;
static std::unordered_map<int, std::string> g_tidToCmdId;

static std::unordered_map<std::string, GTLActiveCmd> g_activeById;  // id  -> full state
static std::deque<GTLActiveCmd> g_respawnQ;

constexpr auto GTL_MIN_TID = 15000000;
constexpr auto GTL_MAX_TID = 16000000; // keep this global so it never collides
static uint32_t g_tidCounter = GTL_MIN_TID; // keep this global so it never collides

// JSON
#include "nlohmann/json.hpp"
using json = nlohmann::json;

//extern int pauseext; // provided by engine
extern bool AppActive;
extern bool automapactive;
extern bool viewactive;
extern gameaction_t gameaction;
extern gameinfo_t gameinfo;
extern player_t players[MAXPLAYERS];

#define DEFAULT_BUFLEN 1024

// --------------------------------------------------------------------------------------
// Helpers (unchanged)
// --------------------------------------------------------------------------------------

static void KillAllMonsters() {
    Net_WriteInt8(DEM_GENERICCHEAT);
    Net_WriteInt8(CHT_MASSACRE);
}

static void KillPlayer() {
    Net_WriteInt8(DEM_SUICIDE);
}

static void SummonActor(int command, int command2, FCommandLine argv)
{
    if (argv.argc() > 1)
    {
        PClassActor* type = PClass::FindActor(argv[1]);
        if (type == nullptr)
        {
            Printf("Unknown actor '%s'\n", argv[1]);
            return;
        }
        Net_WriteInt8(argv.argc() > 2 ? command2 : command);
        Net_WriteString(type->TypeName.GetChars());

        if (argv.argc() > 2)
        {
            Net_WriteInt16(atoi(argv[2])); // angle
            Net_WriteInt16((argv.argc() > 3) ? atoi(argv[3]) : 0); // TID
            Net_WriteInt8((argv.argc() > 4) ? atoi(argv[4]) : 0); // special
            for (int i = 5; i < 10; i++)
            { // args[5]
                Net_WriteInt32((i < argv.argc()) ? atoi(argv[i]) : 0);
            }
        }
    }
}

static void PukeScript(FCommandLine argv)
{
    int argc = argv.argc();

    if (argc < 2 || argc > 6)
    {
        Printf("Usage: puke <script> [arg1] [arg2] [arg3] [arg4]\n");
    }
    else
    {
        int script = atoi(argv[1]);

        if (script == 0)
        { // Script 0 is reserved for Strife support. It is not pukable.
            return;
        }
        int arg[4] = { 0, 0, 0, 0 };
        int argn = min<int>(argc - 2, countof(arg)), i;

        for (i = 0; i < argn; ++i)
        {
            arg[i] = atoi(argv[2 + i]);
        }

        if (script > 0)
        {
            Net_WriteInt8(DEM_RUNSCRIPT);
            Net_WriteInt16(script);
        }
        else
        {
            Net_WriteInt8(DEM_RUNSCRIPT2);
            Net_WriteInt16(-script);
        }

        Net_WriteInt8(argn);
        for (i = 0; i < argn; ++i)
        {
            Net_WriteInt32(arg[i]);
        }
    }
}

char letters[40] = " abcdefghijklmnopqrstuvwxyz0123456789-_";

int getLetterIndex(char letter) {
    letter = tolower(letter);  // convert to lowercase
    for (int i = 0; i < 40; ++i) {
        if (letter == letters[i]) {
            return i;
        }
    }
    return 0;
}

struct GTLActor {
    int tid = 0;
    std::string id;
    std::string name = "";
};

std::vector<GTLActor> gtlActors;

TObjPtr<AActor*>    activator;
FLevelLocals* Level;

static int g_last_evt_seq = 0;

static std::unique_ptr<GTLWsClient> g_ws;

static constexpr int GTL_REASON_LEVEL_RELOAD = 4;

//static AActor* FindByTid(int tid) {
//    auto it = Level->GetActorIterator(tid);
//    AActor* actor;
//    actor = it.Next();
//    return actor;
//}

static AActor* FindByTid(int tid) {
    if (tid <= 0) return nullptr;
    if (gamestate != GS_LEVEL) return nullptr;
    if (!players[0].mo) return nullptr;

    FLevelLocals* lev = players[0].mo->Level;   // <- always the current map
    if (!lev) return nullptr;

    auto it = lev->GetActorIterator(tid);
    return it.Next(); // nullptr if none
}

// --------------------------------------------------------------------------------------
// Gate: only process when gameplay is in a safe state
// --------------------------------------------------------------------------------------

int okToProcessCommands() {
    // if (!AppActive) return 0;

    if (gamestate != GS_LEVEL)            return 0;
    if (menuactive != MENU_Off)           return 0;
    if (automapactive)                    return 0;
    if (!viewactive)                      return 0;
    if (gameaction != ga_nothing)         return 0;
    if (pauseext)                         return 0;
    if (players[0].mo == NULL)            return 0;
    if (players[0].mo->health <= 0)       return 0;
    return 1;
}

void GTL_TickRespawns() {
    if (!okToProcessCommands()) return;
    if (g_respawnQ.empty()) return;

    GTLActiveCmd st = g_respawnQ.front();
    g_respawnQ.pop_front();

    uint32_t newTid = ++g_tidCounter;
    if (g_tidCounter > GTL_MAX_TID) {
        newTid = GTL_MIN_TID;
    }
    st.tid = newTid;

    {
        std::lock_guard<std::mutex> lk(g_tidMapMu);
        g_tidToCmdId[newTid] = st.id;
        g_activeById[st.id] = st;
    }

    const int idx0 = getLetterIndex(st.nickname.empty() ? ' ' : st.nickname[0]);

    {
        std::string tcmd = "puke -250 " + std::to_string(newTid) + " " +
            std::to_string(idx0) + " " + std::to_string(st.objectIdx) + " " +
            std::to_string(st.flFriendly);
        PukeScript(FCommandLine(tcmd.c_str()));
    }

    for (int i = 1; i < (int)st.nickname.size(); ++i)
    {
        int lIndex = getLetterIndex(st.nickname[i]);
        std::string ccmd = "puke -269 " + std::to_string(newTid) + " " +
            std::to_string(i) + " " + std::to_string(lIndex);
        PukeScript(FCommandLine(ccmd.c_str()));
    }

    {
        std::string ccmd2 = "puke -259 " + std::to_string(newTid) + " " +
            std::to_string(st.nickname.size()) + " " +
            std::to_string(st.kindHealthParam) + " " +
            std::to_string(st.respawnTimer);
        PukeScript(FCommandLine(ccmd2.c_str()));
    }
}

bool IsGTLHudId(uint32_t id) {
    return id >= GTL_MIN_TID && id <= GTL_MAX_TID;
}

void GTL_WipeThings() {
    const int64_t now = GTLWsClient::nowMs();

    std::vector<GTLActiveCmd> snapshot;
    {
        std::lock_guard<std::mutex> lk(g_tidMapMu);
        snapshot.reserve(g_activeById.size());
        for (auto& kv : g_activeById) snapshot.push_back(kv.second);

        //// IMPORTANT: clear current map bookkeeping
        g_activeById.clear();
        g_tidToCmdId.clear();
    }

    g_respawnQ.clear();

    // Build respawn list
    for (auto& st : snapshot)
    {
        AActor* a = FindByTid(st.tid);
        if (!a) {
            // It’s already gone (picked up, died, timed out, etc.)
            // If you want: send played here with a "gone_on_wipe" reason.
            continue;
        }

        // If it's a monster command and it's already dead, don't respawn it
        if (st.kindHealthParam > 0 && a->health <= 0)
            continue;

        const int remSec = a->gtlatimer;

        if (st.aTimer > 0) // command was a timed thing
        {
            if (remSec <= 0)
                continue; // expired already

            st.respawnTimer = remSec;
        }
        else
        {
            // "no timer" mode
            st.respawnTimer = 0;
        }

        // preserve current monster health (optional)
        if (st.kindHealthParam > 0)
            st.kindHealthParam = std::max(1, (int)a->health);

        g_respawnQ.push_back(st);
    }

    //gtlActors.clear(); // optional cleanup
}

void GTL_PollAcsRemovedEvents()
{
    const int seq = gtl_evt_seq;

    /*if (seq > 0) {
        g_last_evt_seq = seq;
    }*/
    if (seq == g_last_evt_seq) return;
    g_last_evt_seq = seq;

    const int tid = gtl_evt_tid;
    const int reason = gtl_evt_reason; // 1=timeout, 2=death_fade

    //GTL_WipeThings();

    std::string cmdId;
    {
        std::lock_guard<std::mutex> lk(g_tidMapMu);
        auto it = g_tidToCmdId.find(tid);
        if (it != g_tidToCmdId.end())
        {
            cmdId = it->second;
            g_tidToCmdId.erase(it); // cleanup so map doesn’t grow forever
        }
    }

    if (!cmdId.empty() && g_ws)
    {
        // you implement this similar to notifyPlayed()
        g_ws->notifyPlayed(cmdId, reason);
        Printf("GTL removed event: tid=%d reason=%d command=%s\n", tid, reason, cmdId);
    }
    else
    {
        // fallback debug
        
    }
}

// --------------------------------------------------------------------------------------
// Core command handling (kept same, now calls g_ws->notifyPlayed(id))
// --------------------------------------------------------------------------------------

int processCommand(json j) {
    try {

        if (!okToProcessCommands())
            return 1; // defer / retry later

        /*std::string id = j["id"].template get<std::string>();*/
        std::string id;
        if (j.contains("id"))
        {
            if (j["id"].is_string()) id = j["id"].get<std::string>();
            else if (j["id"].is_number_integer()) id = std::to_string(j["id"].get<long long>());
            else id = j["id"].dump();
        }
        std::string command = j["command"].template get<std::string>();
        std::string nickname = j["nickname"].template get<std::string>();
        int health = j["health"].template get<int>();
        int objectIdx = j["oIdx"].template get<int>();
        int aTimer = j["aTimer"].template get<int>();
        int flFriendly = j["flFriendly"].template get<int>();

        uint32_t newTid = ++g_tidCounter;
        if (g_tidCounter >= GTL_MAX_TID) {
            newTid = GTL_MIN_TID;
        }

        GTLActiveCmd st;
        st.id = id;
        st.tid = newTid;
        st.nickname = nickname;
        st.objectIdx = objectIdx;
        st.flFriendly = flFriendly;
        st.kindHealthParam = health;
        st.aTimer = aTimer;
        st.expireMs = (aTimer > 0) ? (GTLWsClient::nowMs() + (int64_t)aTimer * 1000) : 0;

        if (nickname.empty()) nickname = " ";

        {
            std::lock_guard<std::mutex> lk(g_tidMapMu);
            g_tidToCmdId[newTid] = id;
            g_activeById[id] = st;
        }

        GTLActor tActor;
        tActor.id = id;
        tActor.name = nickname;
        tActor.tid = newTid;
        gtlActors.push_back(tActor);

        int idx = getLetterIndex(tActor.name[0]);

        //// Wait until allowed to process
        //while (!okToProcessCommands()) {
        //    Sleep(100);
        //}

        // initial setup (first letter, type, friendly)
        {
            std::string tcmd = "puke -250 " + std::to_string(newTid) + " " +
                std::to_string(idx) + " " +
                std::to_string(objectIdx) + " " +
                std::to_string(flFriendly);
            FCommandLine argv3(tcmd.c_str());
            PukeScript(argv3);
        }

        // push remaining nickname chars
        for (int i = 1; i < (int)nickname.size(); ++i) {
            int lIndex = getLetterIndex(nickname[i]);
            std::string gcmd = "puke -269 " + std::to_string(newTid) + " " +
                std::to_string(i) + " " + std::to_string(lIndex);
            FCommandLine argv(gcmd.c_str());
            PukeScript(argv);
        }

        // finalize (length, health, timer)
        {
            std::string gcmd2 = "puke -259 " + std::to_string(newTid) + " " +
                std::to_string(nickname.size()) + " " +
                std::to_string(health) + " " +
                std::to_string(aTimer);
            FCommandLine argv2(gcmd2.c_str());
            PukeScript(argv2);
        }

        // tell the WS client we played the command so it can send {type:"played"}
        //if (g_ws && !id.empty()) g_ws->notifyPlayed(id);
    }
    catch (const std::exception& e) {
        Printf("processCommand error: %s\n", e.what());
        return -1;
    }
    catch (...) {
        Printf("processCommand error: unknown\n");
        return -1;
    }
    return 0;
}

// --------------------------------------------------------------------------------------
// Game code → server identity
// --------------------------------------------------------------------------------------

std::string getGameCode() {
    std::string gameCode = "";
    if (gameinfo.gametype == GAME_Doom && gameinfo.flags == 1552) {
        gameCode = "doom";
    }
    else if (gameinfo.gametype == GAME_Doom && gameinfo.flags == 1553) {
        gameCode = "doom2";
    }
    else if (gameinfo.gametype == GAME_Heretic) {
        gameCode = "heretic";
    }
    return gameCode;
}

// --------------------------------------------------------------------------------------
// Entry: start the WS client in the background using the GTL class
// --------------------------------------------------------------------------------------

int GTL_InitSocket(const char* /*host*/, const char* /*port*/) {
    g_ws = std::make_unique<GTLWsClient>(
        // processCommand
        [](const nlohmann::json& j) -> int { return processCommand(j); },
        // okToProcess
        []() -> int { return okToProcessCommands(); },
        // getGameCode
        []() -> std::string { return getGameCode(); }
    );

    g_ws->start("ws://127.0.0.1:14752");
    return 0;
}

// --------------------------------------------------------------------------------------
// URL encode helper
// --------------------------------------------------------------------------------------

FString GTL_URLencode(const char* s)
{
    const char* unreserved = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_.~";

    FString out;
    for (size_t i = 0; s[i]; i++)
    {
        if (strchr(unreserved, s[i]))
        {
            out += s[i];
        }
        else
        {
            out.AppendFormat("%%%02X", s[i] & 255);
        }
    }
    return out;
}