#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace craft
{
// One non-blocking IPv4 TCP connection. PSP sockets share the browser's network
// lifetime. beginNetwork opens the system Wi-Fi dialog; DNS may take a few seconds.
class Transport final
{
  public:
    enum class State
    {
        Idle,
        Wifi,
        Resolving,
        Connecting,
        Open,
        Failed
    };
    // Creates an idle transport.
    Transport() = default;
    // Closes any open connection.
    ~Transport();
    // Copying is disabled because the socket is owned uniquely.
    Transport(const Transport &) = delete;
    // Copy assignment is disabled because the socket is owned uniquely.
    Transport &operator=(const Transport &) = delete;
    // Starts a non-blocking connection to the given host and port.
    bool begin(const std::string &host, std::uint16_t port);
    // Keeps the platform network alive while the browser and game use sockets.
    bool beginNetwork();
    // System Wi-Fi input/rendering owns the screen while this is true.
    static bool wifiDialogActive();
    // Advances platform networking, connect, and pending sends.
    void update();
    // Queues bytes for sending without blocking.
    bool queue(const std::uint8_t *data, std::size_t size);
    // Reads available bytes; >0 bytes, 0 when no data is ready, or -1 on failure.
    int receive(std::uint8_t *output, std::size_t capacity);
    // Closes the socket and releases platform resources.
    void close();
    // Returns the current connection state.
    State state() const
    {
        return state_;
    }
    // Returns the last transport error message.
    const std::string &error() const
    {
        return error_;
    }
    // Returns the number of bytes still buffered for sending.
    std::size_t bufferedBytes() const
    {
        return send_.capacity();
    }

  private:
    // Creates and starts the platform networking stack.
    bool initialize();
    // Resolves the host and starts a non-blocking connect.
    bool connectSocket();
#if defined(__PSP__)
    void *resolverJob_ = nullptr;
#endif
    // Records an error and closes the connection.
    void fail(const char *message);
    // Releases platform networking resources.
    void shutdownPlatform();
    std::intptr_t socket_ = -1;
    State state_ = State::Idle;
    std::string host_, error_;
    std::uint16_t port_ = 0;
    std::vector<std::uint8_t> send_;
    std::size_t sent_ = 0;
    double deadline_ = 0;
    bool initialized_ = false;
    bool wifiReady_ = false;
    void *platform_ = nullptr;
};
} // namespace craft
