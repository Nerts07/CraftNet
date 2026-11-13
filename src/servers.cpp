#include "servers.hpp"
#include "protocol.hpp"
#include "text.hpp"
#include "wire.hpp"
#include <raylib.h>
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>

namespace craft
{
namespace
{
constexpr std::size_t StatusLimit = 65536;

// Streaming JSON reader: no DOM and no allocation for favicon/player samples.
class StatusJson
{
  public:
    StatusJson(const std::string &json, ServerEntry &server) : text_(json), server_(server)
    {
    }
    bool parse()
    {
        return value(0, Root) && (space(), at_ == text_.size());
    }

  private:
    enum Mode
    {
        Ignore,
        Root,
        Description,
        Players,
        Version
    };
    void space()
    {
        while (at_ < text_.size() &&
               (text_[at_] == ' ' || text_[at_] == '\t' || text_[at_] == '\r' || text_[at_] == '\n'))
        {
            ++at_;
        }
    }
    bool take(char c)
    {
        space();
        if (at_ == text_.size() || text_[at_] != c)
        {
            return false;
        }
        ++at_;
        return true;
    }
    int hex4()
    {
        int result = 0;
        for (int i = 0; i < 4; ++i)
        {
            if (at_ == text_.size())
            {
                return -1;
            }
            const char c = text_[at_++];
            const int n = c >= '0' && c <= '9'   ? c - '0'
                          : c >= 'a' && c <= 'f' ? c - 'a' + 10
                          : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                                 : -1;
            if (n < 0)
            {
                return -1;
            }
            result = result * 16 + n;
        }
        return result;
    }
    bool string(std::string *output, std::size_t limit)
    {
        if (!take('"'))
        {
            return false;
        }
        while (at_ < text_.size())
        {
            unsigned c = static_cast<unsigned char>(text_[at_++]);
            if (c == '"')
            {
                return true;
            }
            if (c < 32)
            {
                return false;
            }
            if (c == '\\')
            {
                if (at_ == text_.size())
                {
                    return false;
                }
                c = static_cast<unsigned char>(text_[at_++]);
                if (c == 'u')
                {
                    int code = hex4();
                    if (code < 0)
                    {
                        return false;
                    }
                    if (code >= 0xd800 && code <= 0xdbff)
                    {
                        if (at_ + 2 > text_.size() || text_[at_++] != '\\' || text_[at_++] != 'u')
                        {
                            return false;
                        }
                        const int low = hex4();
                        if (low < 0xdc00 || low > 0xdfff)
                        {
                            return false;
                        }
                        code = 0x10000 + ((code - 0xd800) << 10) + low - 0xdc00;
                    }
                    if (output && output->size() + 4 <= limit)
                    {
                        appendUtf8(*output, static_cast<unsigned>(code));
                    }
                    continue;
                }
                if (c == 'n')
                {
                    c = '\n';
                }
                else if (c == 't' || c == 'r' || c == 'b' || c == 'f')
                {
                    c = ' ';
                }
                else if (c != '"' && c != '\\' && c != '/')
                {
                    return false;
                }
            }
            if (output && output->size() < limit)
            {
                output->push_back(static_cast<char>(c));
            }
        }
        return false;
    }
    bool number(int &result)
    {
        space();
        bool negative = false;
        if (at_ < text_.size() && text_[at_] == '-')
        {
            negative = true;
            ++at_;
        }
        if (at_ == text_.size() || text_[at_] < '0' || text_[at_] > '9')
        {
            return false;
        }
        result = 0;
        while (at_ < text_.size() && text_[at_] >= '0' && text_[at_] <= '9')
        {
            const int n = text_[at_++] - '0';
            if (result > 100000000)
            {
                return false;
            }
            result = result * 10 + n;
        }
        if (negative)
        {
            result = -result;
        }
        return true;
    }
    bool value(int depth, Mode mode)
    {
        if (depth > 24 || ++nodes_ > 8192)
        {
            return false;
        }
        space();
        if (at_ == text_.size())
        {
            return false;
        }
        if (text_[at_] == '{')
        {
            ++at_;
            if (take('}'))
            {
                return true;
            }
            do
            {
                std::string key;
                if (!string(&key, 32) || !take(':'))
                {
                    return false;
                }
                if ((mode == Players && (key == "online" || key == "max")) || (mode == Version && key == "protocol"))
                {
                    int n;
                    if (!number(n))
                    {
                        return false;
                    }
                    if (key == "online")
                    {
                        server_.online = std::max(0, n);
                    }
                    else if (key == "max")
                    {
                        server_.maximum = std::max(0, n);
                    }
                    else
                    {
                        server_.protocol = n;
                    }
                }
                else if (mode == Root && key == "name")
                {
                    if (!string(&server_.name, 96))
                    {
                        return false;
                    }
                }
                else
                {
                    Mode next = Ignore;
                    if (mode == Root)
                    {
                        next = key == "description" ? Description
                               : key == "players"   ? Players
                               : key == "version"   ? Version
                                                    : Ignore;
                    }
                    else if (mode == Description &&
                             (key == "text" || key == "extra" || key == "with" || key == "fallback"))
                    {
                        next = Description;
                    }
                    if (!value(depth + 1, next))
                    {
                        return false;
                    }
                }
            } while (take(','));
            return take('}');
        }
        if (text_[at_] == '[')
        {
            ++at_;
            if (take(']'))
            {
                return true;
            }
            do
            {
                if (!value(depth + 1, mode == Description ? Description : Ignore))
                {
                    return false;
                }
            } while (take(','));
            return take(']');
        }
        if (text_[at_] == '"')
        {
            return string(mode == Description ? &server_.description : nullptr, 384);
        }
        for (const char *literal : {"true", "false", "null"})
        {
            const auto size = std::strlen(literal);
            if (text_.compare(at_, size, literal) == 0)
            {
                at_ += size;
                return true;
            }
        }
        // Unknown numeric fields may be decimal/exponent values.
        const auto start = at_;
        while (at_ < text_.size() && std::strchr("0123456789+-.eE", text_[at_]))
        {
            ++at_;
        }
        return start != at_;
    }
    const std::string &text_;
    ServerEntry &server_;
    std::size_t at_ = 0;
    unsigned nodes_ = 0;
};

bool readFile(const char *name, wire::Bytes &bytes)
{
    FILE *file = std::fopen(name, "rb");
    if (!file)
    {
        return false;
    }
    bool valid = std::fseek(file, 0, SEEK_END) == 0;
    const long size = std::ftell(file);
    valid = valid && size >= 8 && size <= 32768 && std::fseek(file, 0, SEEK_SET) == 0;
    if (valid)
    {
        bytes.resize(static_cast<std::size_t>(size));
        valid = std::fread(bytes.data(), 1, bytes.size(), file) == bytes.size();
    }
    std::fclose(file);
    return valid;
}

bool configLine(std::ifstream &input, std::string &value, std::size_t limit)
{
    char line[258];
    if (!input.getline(line, static_cast<std::streamsize>(limit + 3)))
    {
        return false;
    }
    value = line;
    if (!value.empty() && value.back() == '\r')
    {
        value.pop_back();
    }
    return value.size() <= limit;
}
} // namespace

void ServerList::load()
{
    cancel();
    count_ = 0;
    wire::Bytes data;
    bool damaged = false;
    for (const char *filename : {"servers.dat", "servers.dat.bak"})
    {
        if (!readFile(filename, data))
        {
            continue;
        }
        wire::Reader reader(data.data(), data.size());
        std::uint16_t count;
        const bool hasCachedAddress = std::memcmp(data.data(), "CCS2", 4) == 0;
        if ((hasCachedAddress || std::memcmp(data.data(), "CCS1", 4) == 0) && reader.skip(4) && reader.u16(count) &&
            count <= Capacity &&
            reader.str(username, 16))
        {
            bool valid = true;
            for (std::size_t i = 0; i < count; ++i)
            {
                auto &entry = entries_[i];
                std::string oldCachedAddress;
                valid = valid && reader.str(entry.host, 255) && reader.u16(entry.port) && reader.str(entry.alias, 96) &&
                        reader.str(entry.name, 96) && reader.str(entry.description, 384) &&
                        (!hasCachedAddress || reader.str(oldCachedAddress, 15)) && !entry.host.empty() && entry.port != 0;
            }
            if (valid && reader.remaining() == 0)
            {
                count_ = count;
                return;
            }
        }
        damaged = true;
    }
    entries_ = {};
    username = "PSPPlayer";
    if (damaged)
    {
        error_ = "Cannot read saved list; server.cfg loaded";
    }
    // Import the existing connection once, so an upgrade preserves its address.
    std::ifstream config("server.cfg");
    ServerEntry entry;
    std::string port;
    if (configLine(config, entry.host, 255) && !entry.host.empty())
    {
        configLine(config, port, 5);
        char *end = nullptr;
        const long value = std::strtol(port.c_str(), &end, 10);
        if (end && !*end && value > 0 && value <= 65535)
        {
            entry.port = static_cast<std::uint16_t>(value);
        }
        std::string name;
        if (configLine(config, name, 16) && !name.empty())
        {
            username = name;
        }
        if (!entry.host.empty())
        {
            entries_[count_++] = entry;
            dirty_ = true;
        }
    }
}

bool ServerList::save()
{
    wire::Bytes bytes{'C', 'C', 'S', '1'};
    wire::u16(bytes, static_cast<std::uint16_t>(count_));
    wire::string(bytes, username);
    for (std::size_t i = 0; i < count_; ++i)
    {
        const auto &entry = entries_[i];
        wire::string(bytes, entry.host);
        wire::u16(bytes, entry.port);
        wire::string(bytes, entry.alias);
        wire::string(bytes, entry.name);
        wire::string(bytes, entry.description);
    }
    FILE *file = std::fopen("servers.dat.tmp", "wb");
    if (!file)
    {
        error_ = "Cannot write servers.dat (storage unavailable)";
        return false;
    }
    bool valid = std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
    valid = std::fflush(file) == 0 && valid;
    valid = std::fclose(file) == 0 && valid;
    if (valid)
    {
        // Same-directory replacement with recovery on systems whose rename cannot overwrite.
        bool backedUp = false;
        FILE *previous = std::fopen("servers.dat", "rb");
        if (previous)
        {
            std::fclose(previous);
            valid = std::remove("servers.dat.bak") == 0 || errno == ENOENT;
            if (valid)
            {
                backedUp = std::rename("servers.dat", "servers.dat.bak") == 0;
                valid = backedUp;
            }
        }
        if (valid)
        {
            valid = std::rename("servers.dat.tmp", "servers.dat") == 0;
        }
        if (!valid && backedUp)
        {
            std::rename("servers.dat.bak", "servers.dat");
        }
    }
    if (!valid)
    {
        std::remove("servers.dat.tmp");
        error_ = "Cannot save server list; previous file kept";
        return false;
    }
    dirty_ = false;
    error_.clear();
    return true;
}

bool ServerList::put(std::size_t index, const ServerEntry &entry)
{
    if (index > count_ || index >= Capacity || entry.host.empty() || entry.host.size() > 255 || !entry.port ||
        entry.alias.size() > 96)
    {
        error_ = "Invalid address, port, or full server list";
        return false;
    }
    cancel();
    entries_[index] = entry;
    entries_[index].status = ServerEntry::Status::Unknown;
    if (index == count_)
    {
        ++count_;
    }
    dirty_ = true;
    return save();
}

bool ServerList::erase(std::size_t index)
{
    if (index >= count_)
    {
        return false;
    }
    cancel();
    for (std::size_t i = index + 1; i < count_; ++i)
    {
        entries_[i - 1] = std::move(entries_[i]);
    }
    entries_[--count_] = {};
    dirty_ = true;
    return save();
}

void ServerList::cancel()
{
    query_.close();
    std::vector<std::uint8_t>().swap(response_);
    refreshing_ = false;
    requested_ = false;
    for (std::size_t i = 0; i < count_; ++i)
    {
        if (entries_[i].status == ServerEntry::Status::Querying || entries_[i].status == ServerEntry::Status::Waiting)
        {
            entries_[i].status = ServerEntry::Status::Unknown;
        }
    }
    if (dirty_)
    {
        save();
    }
}

void ServerList::refresh()
{
    cancel();
    error_.clear();
    current_ = 0;
    refreshing_ = count_ > 0;
    for (std::size_t i = 0; i < count_; ++i)
    {
        entries_[i].status = ServerEntry::Status::Waiting;
    }
    if (!count_)
    {
        save();
    }
}

void ServerList::finish(const char *failure)
{
    auto &entry = entries_[current_];
    entry.statusError = failure ? failure : "";
    entry.status = failure                               ? ServerEntry::Status::Offline
                   : entry.protocol != protocol::Java263 ? ServerEntry::Status::Incompatible
                                                         : ServerEntry::Status::Online;
    query_.close();
    response_.clear();
    requested_ = false;
    ++current_;
    if (current_ == count_)
    {
        refreshing_ = false;
        std::vector<std::uint8_t>().swap(response_);
        if (dirty_)
        {
            save();
        }
    }
}

void ServerList::update()
{
    if (!refreshing_)
    {
        return;
    }
    auto &entry = entries_[current_];
    if (query_.state() == Transport::State::Idle)
    {
        entry.status = ServerEntry::Status::Querying;
        if (!query_.begin(entry.host, entry.port))
        {
            finish(query_.error().c_str());
            return;
        }
    }
    query_.update();
    if (query_.state() == Transport::State::Failed)
    {
        finish(query_.error().c_str());
        return;
    }
    if (query_.state() != Transport::State::Open)
    {
        return;
    }
    if (!requested_)
    {
        wire::Bytes frame;
        if (!protocol::handshake(entry.host, entry.port, 1, frame) || !query_.queue(frame.data(), frame.size()))
        {
            finish("Cannot send status handshake");
            return;
        }
        const std::uint8_t request[2] = {1, 0};
        if (!query_.queue(request, sizeof(request)))
        {
            finish("Cannot send status request");
            return;
        }
        response_.reserve(StatusLimit + 5);
        requested_ = true;
        deadline_ = GetTime() + 5;
    }
    if (GetTime() > deadline_)
    {
        finish("Server status response timed out");
        return;
    }
    std::uint8_t incoming[2048];
    // Bound work per frame, even when the peer continuously supplies bytes.
    for (int iteration = 0; iteration < 8; ++iteration)
    {
        const int received = query_.receive(incoming, sizeof(incoming));
        if (received < 0)
        {
            finish(query_.error().c_str());
            return;
        }
        if (!received)
        {
            break;
        }
        if (response_.size() + static_cast<std::size_t>(received) > StatusLimit + 5)
        {
            finish("Server status response too large");
            return;
        }
        response_.insert(response_.end(), incoming, incoming + received);
        std::size_t offset = 0;
        std::int32_t length;
        const auto decoded = protocol::readVarInt(response_.data(), response_.size(), offset, length);
        if (decoded == protocol::DecodeResult::NeedMore)
        {
            continue;
        }
        if (decoded != protocol::DecodeResult::Packet || length < 1 || length > static_cast<std::int32_t>(StatusLimit))
        {
            finish("Invalid server status packet length");
            return;
        }
        if (response_.size() - offset < static_cast<std::size_t>(length))
        {
            continue;
        }
        wire::Reader reader(response_.data() + offset, static_cast<std::size_t>(length));
        std::int32_t id;
        std::string json;
        ServerEntry next;
        if (!reader.var(id) || id != 0 || !reader.str(json, StatusLimit) || reader.remaining() ||
            !StatusJson(json, next).parse())
        {
            finish("Invalid server status response");
            return;
        }
        next.description = displayText(next.description, 384);
        next.name = displayText(next.name, 96);
        if (next.name.empty())
        {
            next.name = displayText(next.description.substr(0, next.description.find('\n')), 96);
        }
        dirty_ |= next.name != entry.name || next.description != entry.description;
        entry.name = std::move(next.name);
        entry.description = std::move(next.description);
        entry.online = next.online;
        entry.maximum = next.maximum;
        entry.protocol = next.protocol;
        finish();
        return;
    }
}
} // namespace craft
