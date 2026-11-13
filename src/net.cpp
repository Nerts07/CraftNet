#if defined(__PSP__)
// sceNetInetSelect uses 256-bit masks. The default newlib fd_set has 64 bits.
#define FD_SETSIZE 256
#endif

#include "net.hpp"
#include "psp_dialog.hpp"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <new>

#if defined(__PSP__)
#include <pspkernel.h>
#include <pspnet.h>
#include <pspnet_inet.h>
#include <pspnet_apctl.h>
#include <pspnet_resolver.h>
#include <psputility.h>
#include <netinet/in.h>
#include <errno.h>
#elif defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <unistd.h>
#endif

extern "C" double GetTime(void);

namespace craft
{
namespace
{
constexpr std::size_t MaxSendBytes = 128 * 1024;
#if defined(__PSP__)
static_assert(sizeof(fd_set) == 32, "PSP select requires 256-bit descriptor sets");

// Smaller priorities run first. Raylib's PSP frame limiter can busy-wait on
// the main thread (priority 32), so the network stack must preempt it.
constexpr int PspNetworkPriority = 30;

struct PspNetwork
{
    pspUtilityNetconfData dialog{};
    pspUtilityNetconfAdhoc adhoc{};
    bool common = false, inetModule = false, net = false, inet = false, ap = false, resolver = false;
    unsigned references = 1;
    bool ready = false;
    bool shuttingDown = false;
    bool dialogActive = false;
};
PspNetwork *sharedNetwork = nullptr;
struct PspResolver
{
    alignas(16) unsigned char buffer[4096];
    std::string host;
    in_addr address{};
    std::atomic<bool> done{false};
    int resolver = -1, thread = -1, result = -1;
    bool started = false;
};

int resolveHost(SceSize, void *argument)
{
    auto *job = *static_cast<PspResolver **>(argument);
    job->result = sceNetResolverStartNtoA(job->resolver, job->host.c_str(), &job->address, 3, 1);
    job->done.store(true, std::memory_order_release);
    return 0;
}

void releaseResolver(void *&value)
{
    auto *job = static_cast<PspResolver *>(value);
    if (!job)
    {
        return;
    }
    if (job->started)
    {
        if (!job->done.load(std::memory_order_acquire))
        {
            sceNetResolverStop(job->resolver);
        }
        sceKernelWaitThreadEnd(job->thread, nullptr);
    }
    if (job->thread >= 0)
    {
        sceKernelDeleteThread(job->thread);
    }
    if (job->resolver >= 0)
    {
        sceNetResolverDelete(job->resolver);
    }
    delete job;
    value = nullptr;
}

// Returns the last PSP networking error.
int lastError()
{
    return sceNetInetGetErrno();
}
// Closes a PSP socket.
void closeSocket(intptr_t s)
{
    sceNetInetClose(static_cast<int>(s));
}
// Returns whether an error means the operation would block.
bool wouldBlock(int e)
{
    return e == EAGAIN || e == EINPROGRESS || e == EALREADY;
}
// Sends bytes through the PSP socket API.
int socketSend(intptr_t s, const void *data, std::size_t size)
{
    return static_cast<int>(sceNetInetSend(static_cast<int>(s), data, size, 0));
}
// Receives bytes through the PSP socket API.
int socketReceive(intptr_t s, void *data, std::size_t size)
{
    return static_cast<int>(sceNetInetRecv(static_cast<int>(s), data, size, 0));
}
#elif defined(_WIN32)
// Returns the last Winsock error.
int lastError()
{
    return WSAGetLastError();
}
// Closes a Winsock socket.
void closeSocket(intptr_t s)
{
    closesocket(static_cast<SOCKET>(s));
}
// Returns whether a Winsock error means the operation would block.
bool wouldBlock(int e)
{
    return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS || e == WSAEALREADY;
}
// Sends bytes through the Winsock API.
int socketSend(intptr_t s, const void *data, std::size_t size)
{
    return ::send(static_cast<SOCKET>(s), static_cast<const char *>(data), static_cast<int>(size), 0);
}
// Receives bytes through the Winsock API.
int socketReceive(intptr_t s, void *data, std::size_t size)
{
    return ::recv(static_cast<SOCKET>(s), static_cast<char *>(data), static_cast<int>(size), 0);
}
#else
// Returns the last POSIX socket error.
int lastError()
{
    return errno;
}
// Closes a POSIX socket.
void closeSocket(intptr_t s)
{
    ::close(static_cast<int>(s));
}
// Returns whether a POSIX error means the operation would block.
bool wouldBlock(int e)
{
    return e == EAGAIN || e == EINPROGRESS || e == EALREADY;
}
// Sends bytes through the POSIX socket API.
int socketSend(intptr_t s, const void *data, std::size_t size)
{
    return static_cast<int>(::send(static_cast<int>(s), data, size, 0));
}
// Receives bytes through the POSIX socket API.
int socketReceive(intptr_t s, void *data, std::size_t size)
{
    return static_cast<int>(::recv(static_cast<int>(s), data, size, 0));
}
#endif
} // namespace

// Closes the socket when the transport is destroyed.
Transport::~Transport()
{
    close();
}

bool Transport::wifiDialogActive()
{
#if defined(__PSP__)
    return sharedNetwork && sharedNetwork->dialogActive;
#else
    return false;
#endif
}

// Records an error and closes the connection.
void Transport::fail(const char *message)
{
#if defined(__PSP__)
    releaseResolver(resolverJob_);
#endif
    if (socket_ != -1)
    {
        closeSocket(socket_);
        socket_ = -1;
    }
    error_ = message;
    state_ = State::Failed;
}

// Creates and starts the platform networking stack.
bool Transport::initialize()
{
#if defined(__PSP__)
    if (sharedNetwork)
    {
        int state = 0;
        if (!sharedNetwork->ready || sceNetApctlGetState(&state) < 0 || state != 4)
        {
            return false;
        }
        ++sharedNetwork->references;
        platform_ = sharedNetwork;
        initialized_ = true;
        wifiReady_ = true;
        return true;
    }
    auto *p = new (std::nothrow) PspNetwork;
    if (!p)
    {
        return false;
    }
    platform_ = p;
    if (sceUtilityLoadNetModule(PSP_NET_MODULE_COMMON) < 0)
    {
        return false;
    }
    p->common = true;
    if (sceUtilityLoadNetModule(PSP_NET_MODULE_INET) < 0)
    {
        return false;
    }
    p->inetModule = true;
    if (sceNetInit(128 * 1024, PspNetworkPriority, 4096, PspNetworkPriority, 4096) < 0)
    {
        return false;
    }
    p->net = true;
    if (sceNetInetInit() < 0)
    {
        return false;
    }
    p->inet = true;
    if (sceNetApctlInit(0x8000, PspNetworkPriority) < 0)
    {
        return false;
    }
    p->ap = true;
    if (sceNetResolverInit() < 0)
    {
        return false;
    }
    p->resolver = true;
    p->dialog.base.size = sizeof(p->dialog);
    p->dialog.base.language = PSP_SYSTEMPARAM_LANGUAGE_ENGLISH;
    sceUtilityGetSystemParamInt(PSP_SYSTEMPARAM_ID_INT_LANGUAGE, &p->dialog.base.language);
    p->dialog.base.buttonSwap = PSP_UTILITY_ACCEPT_CROSS;
    p->dialog.base.graphicsThread = 17;
    p->dialog.base.accessThread = 19;
    p->dialog.base.fontThread = 18;
    p->dialog.base.soundThread = 16;
    p->dialog.action = PSP_NETCONF_ACTION_CONNECTAP;
    p->dialog.adhocparam = &p->adhoc;
    if (sceUtilityNetconfInitStart(&p->dialog) < 0)
    {
        return false;
    }
    p->dialogActive = true;
    sharedNetwork = p;
#elif defined(_WIN32)
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
    {
        return false;
    }
#endif
    initialized_ = true;
    return true;
}

// Releases platform networking resources.
void Transport::shutdownPlatform()
{
#if defined(__PSP__)
    auto *p = static_cast<PspNetwork *>(platform_);
    if (p)
    {
        if (--p->references != 0)
        {
            initialized_ = false;
            platform_ = nullptr;
            return;
        }
        if (p->resolver)
        {
            sceNetResolverTerm();
        }
        if (p->ap)
        {
            sceNetApctlTerm();
        }
        if (p->inet)
        {
            sceNetInetTerm();
        }
        if (p->net)
        {
            sceNetTerm();
        }
        if (p->inetModule)
        {
            sceUtilityUnloadNetModule(PSP_NET_MODULE_INET);
        }
        if (p->common)
        {
            sceUtilityUnloadNetModule(PSP_NET_MODULE_COMMON);
        }
        if (sharedNetwork == p)
        {
            sharedNetwork = nullptr;
        }
        delete p;
    }
#elif defined(_WIN32)
    if (initialized_)
    {
        WSACleanup();
    }
#endif
    initialized_ = false;
    platform_ = nullptr;
}

// Closes the socket and releases platform resources.
void Transport::close()
{
#if defined(__PSP__)
    releaseResolver(resolverJob_);
#endif
    if (socket_ != -1)
    {
        closeSocket(socket_);
        socket_ = -1;
    }
    shutdownPlatform();
    std::vector<std::uint8_t>().swap(send_);
    sent_ = 0;
    state_ = State::Idle;
    wifiReady_ = false;
}

// Starts a non-blocking connection to the given host and port.
bool Transport::beginNetwork()
{
    close();
    host_.clear();
    error_.clear();
    if (!initialize())
    {
        shutdownPlatform();
        fail("Network initialization failed");
        return false;
    }
#if defined(__PSP__)
    state_ = wifiReady_ ? State::Open : State::Wifi;
#else
    state_ = State::Open;
#endif
    return true;
}

// Starts a non-blocking connection to the given host and port.
bool Transport::begin(const std::string &host, std::uint16_t port)
{
    close();
    error_.clear();
    if (host.empty() || host.size() > 255 || !port)
    {
        fail("Invalid server address");
        return false;
    }
    host_ = host;
    port_ = port;
    if (!initialize())
    {
        shutdownPlatform();
        fail("Network initialization failed");
        return false;
    }
#if defined(__PSP__)
    if (!wifiReady_)
    {
        state_ = State::Wifi;
    }
    else if (!connectSocket())
    {
        return false;
    }
#else
    if (!connectSocket())
    {
        return false;
    }
#endif
    return true;
}

// Resolves the host and starts a non-blocking connect.
bool Transport::connectSocket()
{
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port_);
#if defined(__PSP__)
    addr.sin_len = sizeof(addr);
    unsigned a, b, c, d;
    char suffix;
    if (std::sscanf(host_.c_str(), "%u.%u.%u.%u%c", &a, &b, &c, &d, &suffix) == 4 && a < 256 && b < 256 && c < 256 &&
        d < 256)
    {
        addr.sin_addr.s_addr = htonl((a << 24) | (b << 16) | (c << 8) | d);
    }
    else
    {
        auto *job = static_cast<PspResolver *>(resolverJob_);
        if (!job)
        {
            job = new (std::nothrow) PspResolver;
            resolverJob_ = job;
            if (!job)
            {
                fail("Cannot allocate DNS resolver");
                return false;
            }
            job->host = host_;
            if (sceNetResolverCreate(&job->resolver, job->buffer, sizeof(job->buffer)) < 0)
            {
                fail("Cannot create DNS resolver");
                return false;
            }
            job->thread = sceKernelCreateThread("CraftNet DNS", resolveHost, 31, 8192, PSP_THREAD_ATTR_USER, nullptr);
            if (job->thread < 0 || sceKernelStartThread(job->thread, sizeof(job), &job) < 0)
            {
                fail("Cannot start DNS resolver");
                return false;
            }
            job->started = true;
            state_ = State::Resolving;
            deadline_ = GetTime() + 8.0;
            return true;
        }
        if (!job->done.load(std::memory_order_acquire))
        {
            if (GetTime() >= deadline_)
            {
                fail("DNS lookup timed out");
                return false;
            }
            return true;
        }
        const int result = job->result;
        addr.sin_addr = job->address;
        releaseResolver(resolverJob_);
        if (result < 0)
        {
            char message[40];
            std::snprintf(message, sizeof(message), "DNS lookup failed (0x%08X)", static_cast<unsigned>(result));
            fail(message);
            return false;
        }
    }
    socket_ = sceNetInetSocket(AF_INET, SOCK_STREAM, 0);
    if (socket_ < 0)
    {
        socket_ = -1;
        fail("Socket creation failed");
        return false;
    }
    int nonblocking = 1;
    if (sceNetInetSetsockopt(static_cast<int>(socket_), SOL_SOCKET, SO_NONBLOCK, &nonblocking, sizeof(nonblocking)) < 0)
    {
        fail("Cannot set nonblocking mode");
        return false;
    }
    const int result = sceNetInetConnect(static_cast<int>(socket_), reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
#else
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo *results = nullptr;
    if (getaddrinfo(host_.c_str(), nullptr, &hints, &results) != 0 || !results)
    {
        fail("DNS lookup failed");
        return false;
    }
    addr.sin_addr = reinterpret_cast<sockaddr_in *>(results->ai_addr)->sin_addr;
    freeaddrinfo(results);
    auto s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#if defined(_WIN32)
    if (s == INVALID_SOCKET)
    {
        fail("Socket creation failed");
        return false;
    }
    socket_ = static_cast<intptr_t>(s);
    u_long nonblocking = 1;
    if (ioctlsocket(s, FIONBIO, &nonblocking) != 0)
    {
        fail("Cannot set nonblocking mode");
        return false;
    }
#else
    if (s < 0)
    {
        fail("Socket creation failed");
        return false;
    }
    socket_ = s;
    if (fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK) < 0)
    {
        fail("Cannot set nonblocking mode");
        return false;
    }
#endif
    const int result = ::connect(s, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
#endif
    if (result != 0 && !wouldBlock(lastError()))
    {
        fail("Connection failed");
        return false;
    }
    if (result == 0)
    {
        state_ = State::Open;
        return true;
    }
    deadline_ = GetTime() + 15.0;
    state_ = State::Connecting;
    return true;
}

// Advances platform networking, connect, and pending sends.
void Transport::update()
{
#if defined(__PSP__)
    if (state_ == State::Resolving)
    {
        connectSocket();
        return;
    }
    if (state_ == State::Wifi)
    {
        auto *p = static_cast<PspNetwork *>(platform_);
        const int status = sceUtilityNetconfGetStatus();
        if (status == PSP_UTILITY_DIALOG_VISIBLE)
        {
            drawPspDialog(sceUtilityNetconfUpdate);
        }
        else if (status == PSP_UTILITY_DIALOG_QUIT)
        {
            if (!p->shuttingDown)
            {
                p->shuttingDown = sceUtilityNetconfShutdownStart() >= 0;
            }
        }
        else if (p->shuttingDown && (status == PSP_UTILITY_DIALOG_FINISHED || status == PSP_UTILITY_DIALOG_NONE))
        {
            p->dialogActive = false;
            int apState = 0;
            if (p->dialog.base.result != 0 || sceNetApctlGetState(&apState) < 0 || apState != 4)
            {
                fail("Wi-Fi connection cancelled or failed");
                return;
            }
            wifiReady_ = true;
            p->ready = true;
            if (host_.empty())
            {
                state_ = State::Open;
            }
            else
            {
                connectSocket();
            }
        }
        else if (status < 0)
        {
            p->dialogActive = false;
            fail("Wi-Fi dialog failed");
        }
    }
    if (state_ == State::Open && host_.empty() && GetTime() >= deadline_)
    {
        deadline_ = GetTime() + 1.0;
        int state = 0;
        if (sceNetApctlGetState(&state) < 0 || state != 4)
        {
            fail("Wi-Fi disconnected; press Refresh to reconnect");
        }
    }
#endif
    if (state_ == State::Connecting)
    {
        if (GetTime() > deadline_)
        {
            fail("Connection timed out");
            return;
        }
        fd_set writes, errors;
        FD_ZERO(&writes);
        FD_ZERO(&errors);
#if defined(_WIN32)
        FD_SET(static_cast<SOCKET>(socket_), &writes);
        FD_SET(static_cast<SOCKET>(socket_), &errors);
        timeval timeout{};
        int ready = select(0, nullptr, &writes, &errors, &timeout);
#elif defined(__PSP__)
        if (socket_ >= FD_SETSIZE)
        {
            fail("Socket number out of range");
            return;
        }
        FD_SET(static_cast<int>(socket_), &writes);
        FD_SET(static_cast<int>(socket_), &errors);
        SceNetInetTimeval timeout{};
        int ready = sceNetInetSelect(static_cast<int>(socket_) + 1, nullptr, &writes, &errors, &timeout);
#else
        FD_SET(static_cast<int>(socket_), &writes);
        FD_SET(static_cast<int>(socket_), &errors);
        timeval timeout{};
        int ready = select(static_cast<int>(socket_) + 1, nullptr, &writes, &errors, &timeout);
#endif
        if (ready < 0)
        {
            fail("Connection status failed");
            return;
        }
        if (ready)
        {
            int socketError = 0;
#if defined(_WIN32)
            int length = sizeof(socketError);
            int result = getsockopt(static_cast<SOCKET>(socket_), SOL_SOCKET, SO_ERROR,
                                    reinterpret_cast<char *>(&socketError), &length);
#elif defined(__PSP__)
            socklen_t length = sizeof(socketError);
            int result = sceNetInetGetsockopt(static_cast<int>(socket_), SOL_SOCKET, SO_ERROR, &socketError, &length);
#else
            socklen_t length = sizeof(socketError);
            int result = getsockopt(static_cast<int>(socket_), SOL_SOCKET, SO_ERROR, &socketError, &length);
#endif
            if (result < 0 || socketError)
            {
                fail("Server refused connection");
                return;
            }
            state_ = State::Open;
        }
    }
    if (state_ == State::Open && sent_ < send_.size())
    {
        int amount = socketSend(socket_, send_.data() + sent_, send_.size() - sent_);
        if (amount > 0)
        {
            sent_ += static_cast<std::size_t>(amount);
            if (sent_ == send_.size())
            {
                send_.clear();
                sent_ = 0;
            }
        }
        else if (amount == 0 || !wouldBlock(lastError()))
        {
            fail("Network send failed");
        }
    }
}

// Queues bytes for sending without blocking.
bool Transport::queue(const std::uint8_t *data, std::size_t size)
{
    if (state_ != State::Open || (!data && size))
    {
        return false;
    }
    if (sent_)
    {
        send_.erase(send_.begin(), send_.begin() + static_cast<std::ptrdiff_t>(sent_));
        sent_ = 0;
    }
    if (size > MaxSendBytes || send_.size() > MaxSendBytes - size)
    {
        fail("Send queue limit exceeded");
        return false;
    }
    if (size)
    {
        send_.insert(send_.end(), data, data + size);
    }
    return true;
}

// Reads available bytes; 0 means nothing ready, -1 means failure/EOF.
int Transport::receive(std::uint8_t *output, std::size_t capacity)
{
    if (state_ != State::Open || !output || !capacity)
    {
        return -1;
    }
    int received = socketReceive(socket_, output, capacity);
    if (received > 0)
    {
        return received;
    }
    if (received < 0 && wouldBlock(lastError()))
    {
        return 0;
    }
    fail(received == 0 ? "Server closed connection" : "Network receive failed");
    return -1;
}
} // namespace craft
