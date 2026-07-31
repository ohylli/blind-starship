// Debug control server transport: a localhost TCP listener whose commands are executed on
// the game thread and answered as one JSON object per line. This file knows nothing about
// Star Fox state — game-facing commands live in DebugCommands.cpp and everything routes
// through the Ship::Console command registry, so each command is equally available from
// the in-game ImGui console. Wire protocol and rationale: docs/debug-server-plan.md.
//
// Threading: socket threads (one acceptor, one per client) only ferry strings. A client
// thread enqueues a tokenized request and blocks until DebugServer_FrameTick — called from
// GameEngine::StartFrame on the single main-loop thread — drains the queue and fills in
// the response. Game state, CVars, and the event bus are never touched off the game thread.

#include "DebugServer.h"

#ifdef __SWITCH__

void DebugServer_Init() {
}
void DebugServer_Exit() {
}
void DebugServer_FrameTick() {
}

#else

// winsock2.h must precede anything that could pull in windows.h (it defines the guard
// that keeps the incompatible winsock.h out).
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#endif

#include <libultraship.h>
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>

#include <condition_variable>
#include <cctype>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "DebugCommands.h"

#ifdef _WIN32
using SocketT = SOCKET;
static constexpr SocketT kInvalidSocket = INVALID_SOCKET;
static void CloseSock(SocketT sock) {
    closesocket(sock);
}
static void ShutdownSock(SocketT sock) {
    shutdown(sock, SD_BOTH);
}
#else
using SocketT = int;
static constexpr SocketT kInvalidSocket = -1;
static void CloseSock(SocketT sock) {
    close(sock);
}
static void ShutdownSock(SocketT sock) {
    shutdown(sock, SHUT_RDWR);
}
#endif

namespace {

// A single line has to fit a command, not a dump — dumps only ever go the other way.
constexpr size_t kMaxRequestLine = 64 * 1024;

struct Request {
    std::vector<std::string> args;
    std::string responseLine; // full JSON line including the trailing '\n', filled on the game thread
    bool done = false;
};

struct ClientSlot {
    SocketT sock = kInvalidSocket;
    std::thread thread;
    bool closed = false; // socket shut down + closed (by the thread itself or by Stop)
    bool exited = false; // thread function has finished; safe to join and reap
};

std::mutex sMutex;
std::condition_variable sCv;
std::deque<std::shared_ptr<Request>> sQueue;
bool sRunning = false;
SocketT sListenSock = kInvalidSocket;
std::thread sAcceptThread;
std::vector<std::unique_ptr<ClientSlot>> sClients;
bool sWanted = false; // last CVar state acted on, so a failed Start is not retried every frame

std::string OkJson(const std::string& output) {
    nlohmann::json j;
    j["status"] = "ok";
    j["output"] = output;
    return j.dump() + "\n";
}

std::string ErrJson(const std::string& message) {
    nlohmann::json j;
    j["status"] = "error";
    j["error"] = message;
    return j.dump() + "\n";
}

// Whitespace-separated tokens; a double-quoted span groups one token (quotes stripped, no
// escape sequences). This is the space-splitting fix that motivates bypassing Console::Run.
std::vector<std::string> Tokenize(const std::string& line) {
    std::vector<std::string> tokens;
    std::string current;
    bool inQuotes = false;
    bool started = false;
    for (char c : line) {
        if (inQuotes) {
            if (c == '"') {
                inQuotes = false;
            } else {
                current += c;
            }
        } else if (c == '"') {
            inQuotes = true;
            started = true;
        } else if (std::isspace(static_cast<unsigned char>(c))) {
            if (started) {
                tokens.push_back(current);
                current.clear();
                started = false;
            }
        } else {
            current += c;
            started = true;
        }
    }
    if (started) {
        tokens.push_back(current);
    }
    return tokens;
}

// Game thread only. HasCommand must precede GetCommand (map operator[] would insert a
// null handler), and the try/catch is load-bearing: the LUS `set` handler calls
// std::stoi/stoul, which throw on malformed input.
std::string Dispatch(const std::vector<std::string>& args) {
    auto console = Ship::Context::GetInstance()->GetConsole();
    if (!console->HasCommand(args[0])) {
        return ErrJson("unknown command: " + args[0]);
    }
    auto& entry = console->GetCommand(args[0]);
    std::string output;
    int32_t rc;
    try {
        rc = entry.Handler(console, args, &output);
    } catch (const std::exception& e) {
        return ErrJson("command threw: " + std::string(e.what()));
    }
    if (rc == 0) {
        return OkJson(output);
    }
    return ErrJson(output.empty() ? "command failed (code " + std::to_string(rc) + ")" : output);
}

bool SendAll(SocketT sock, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        int n = send(sock, data.data() + sent, static_cast<int>(data.size() - sent), 0);
        if (n <= 0) {
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    return true;
}

void ClientThread(ClientSlot* slot) {
    std::string buffer;
    char chunk[4096];
    bool dead = false;
    while (!dead) {
        int n = recv(slot->sock, chunk, sizeof(chunk), 0);
        if (n <= 0) {
            break;
        }
        buffer.append(chunk, static_cast<size_t>(n));
        if (buffer.find('\n') == std::string::npos && buffer.size() > kMaxRequestLine) {
            break; // oversized line: drop the connection rather than buffer without bound
        }
        size_t pos;
        while (!dead && (pos = buffer.find('\n')) != std::string::npos) {
            std::string line = buffer.substr(0, pos);
            buffer.erase(0, pos + 1);
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            auto args = Tokenize(line);
            if (args.empty()) {
                continue;
            }
            auto request = std::make_shared<Request>();
            request->args = std::move(args);
            std::string response;
            {
                std::unique_lock<std::mutex> lock(sMutex);
                if (!sRunning) {
                    dead = true;
                    break;
                }
                sQueue.push_back(request);
                sCv.wait(lock, [&] { return request->done || !sRunning; });
                response = request->done ? request->responseLine : ErrJson("server shutting down");
            }
            if (!SendAll(slot->sock, response)) {
                dead = true;
            }
        }
    }
    std::lock_guard<std::mutex> lock(sMutex);
    if (!slot->closed) {
        ShutdownSock(slot->sock);
        CloseSock(slot->sock);
        slot->closed = true;
    }
    slot->exited = true;
}

void AcceptThread() {
    for (;;) {
        SocketT client = accept(sListenSock, nullptr, nullptr);
        if (client == kInvalidSocket) {
            break; // listen socket closed by Stop
        }
        std::lock_guard<std::mutex> lock(sMutex);
        if (!sRunning) {
            CloseSock(client);
            break;
        }
        // Reap slots whose threads already finished so long sessions don't accumulate them.
        for (auto it = sClients.begin(); it != sClients.end();) {
            if ((*it)->exited) {
                (*it)->thread.join();
                it = sClients.erase(it);
            } else {
                ++it;
            }
        }
        auto slot = std::make_unique<ClientSlot>();
        slot->sock = client;
        ClientSlot* raw = slot.get();
        slot->thread = std::thread(ClientThread, raw);
        sClients.push_back(std::move(slot));
    }
}

// Game thread. On failure the server just stays off — a busy port must not hurt the game.
bool Start() {
    if (sRunning) {
        return true;
    }
    int port = CVarGetInteger("gDebugServer.Port", 7764);
#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        SPDLOG_WARN("Debug server: WSAStartup failed; server stays off");
        return false;
    }
#endif
    sListenSock = socket(AF_INET, SOCK_STREAM, 0);
    if (sListenSock == kInvalidSocket) {
        SPDLOG_WARN("Debug server: socket() failed; server stays off");
#ifdef _WIN32
        WSACleanup();
#endif
        return false;
    }
#ifndef _WIN32
    // Skip TIME_WAIT on quick restarts. Windows SO_REUSEADDR means something else
    // (bind hijacking) and rebinding after close works there without it.
    int yes = 1;
    setsockopt(sListenSock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
#endif
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    // Loopback only: no remote access, and an explicit 127.0.0.1 bind avoids the Windows
    // Firewall prompt a wildcard bind would trigger.
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(sListenSock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(sListenSock, 4) != 0) {
        SPDLOG_WARN("Debug server: could not listen on 127.0.0.1:{} (port in use?); server stays off", port);
        CloseSock(sListenSock);
        sListenSock = kInvalidSocket;
#ifdef _WIN32
        WSACleanup();
#endif
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(sMutex);
        sRunning = true;
    }
    sAcceptThread = std::thread(AcceptThread);
    SPDLOG_INFO("Debug server listening on 127.0.0.1:{}", port);
    return true;
}

// Game thread (engine shutdown and the runtime CVar toggle). Idempotent.
void Stop() {
    {
        std::lock_guard<std::mutex> lock(sMutex);
        if (!sRunning) {
            return;
        }
        sRunning = false;
    }
    sCv.notify_all(); // waiters answer "server shutting down" and unwind
    // shutdown-then-close is the portable way to kick threads blocked in accept/recv.
    ShutdownSock(sListenSock);
    CloseSock(sListenSock);
    {
        std::lock_guard<std::mutex> lock(sMutex);
        for (auto& client : sClients) {
            if (!client->closed) {
                ShutdownSock(client->sock);
                CloseSock(client->sock);
                client->closed = true;
            }
        }
    }
    sAcceptThread.join();
    for (auto& client : sClients) {
        client->thread.join();
    }
    sClients.clear();
    sQueue.clear();
    sListenSock = kInvalidSocket;
#ifdef _WIN32
    WSACleanup();
#endif
    SPDLOG_INFO("Debug server stopped");
}

} // namespace

void DebugServer_Init() {
    // Dotted names: the config file nests on '.', so a scalar "gDebugServer" could not
    // coexist with "gDebugServer.Port" in starship.cfg.json.
    CVarRegisterInteger("gDebugServer.Enabled", 0);
    CVarRegisterInteger("gDebugServer.Port", 7764);
    // Commands register unconditionally so the ImGui console has them even with the
    // socket server off.
    DebugCommands_Register();
    sWanted = CVarGetInteger("gDebugServer.Enabled", 0) != 0;
    if (sWanted) {
        Start();
    }
}

void DebugServer_Exit() {
    Stop();
}

void DebugServer_FrameTick() {
    // Runtime toggle: `set gDebugServer.Enabled 1/0` takes effect on the next frame. A
    // port change applies on the next off->on transition.
    bool want = CVarGetInteger("gDebugServer.Enabled", 0) != 0;
    if (want != sWanted) {
        sWanted = want;
        if (want) {
            Start();
        } else {
            Stop();
        }
    }

    std::deque<std::shared_ptr<Request>> batch;
    {
        std::lock_guard<std::mutex> lock(sMutex);
        if (sQueue.empty()) {
            return;
        }
        batch.swap(sQueue);
    }
    for (auto& request : batch) {
        request->responseLine = Dispatch(request->args); // outside the lock: handlers can be slow
    }
    {
        std::lock_guard<std::mutex> lock(sMutex);
        for (auto& request : batch) {
            request->done = true;
        }
    }
    sCv.notify_all();
}

#endif // __SWITCH__
