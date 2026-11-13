#include "offline_uuid.hpp"
#include <cstring>

// Adapted from mcprotocol.c. See the third-party attribution in README.md.
namespace craft
{
// Builds the offline-mode UUID for a player name.
bool offlineUuid(const std::string &username, std::array<std::uint8_t, 16> &uuid)
{
    if (username.empty() || username.size() > 16)
    {
        return false;
    }
    for (unsigned char c : username)
    {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'))
        {
            return false;
        }
    }
    static const unsigned char shifts[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                                             5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
                                             4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                                             6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
    static const std::uint32_t k[64] = {
        0xd76aa478U, 0xe8c7b756U, 0x242070dbU, 0xc1bdceeeU, 0xf57c0fafU, 0x4787c62aU, 0xa8304613U, 0xfd469501U,
        0x698098d8U, 0x8b44f7afU, 0xffff5bb1U, 0x895cd7beU, 0x6b901122U, 0xfd987193U, 0xa679438eU, 0x49b40821U,
        0xf61e2562U, 0xc040b340U, 0x265e5a51U, 0xe9b6c7aaU, 0xd62f105dU, 0x02441453U, 0xd8a1e681U, 0xe7d3fbc8U,
        0x21e1cde6U, 0xc33707d6U, 0xf4d50d87U, 0x455a14edU, 0xa9e3e905U, 0xfcefa3f8U, 0x676f02d9U, 0x8d2a4c8aU,
        0xfffa3942U, 0x8771f681U, 0x6d9d6122U, 0xfde5380cU, 0xa4beea44U, 0x4bdecfa9U, 0xf6bb4b60U, 0xbebfbc70U,
        0x289b7ec6U, 0xeaa127faU, 0xd4ef3085U, 0x04881d05U, 0xd9d4d039U, 0xe6db99e5U, 0x1fa27cf8U, 0xc4ac5665U,
        0xf4292244U, 0x432aff97U, 0xab9423a7U, 0xfc93a039U, 0x655b59c3U, 0x8f0ccc92U, 0xffeff47dU, 0x85845dd1U,
        0x6fa87e4fU, 0xfe2ce6e0U, 0xa3014314U, 0x4e0811a1U, 0xf7537e82U, 0xbd3af235U, 0x2ad7d2bbU, 0xeb86d391U};
    const std::string text = "OfflinePlayer:" + username;
    std::uint8_t block[64]{};
    std::memcpy(block, text.data(), text.size());
    block[text.size()] = 0x80;
    const std::uint64_t bits = text.size() * 8;
    for (int i = 0; i < 8; ++i)
    {
        block[56 + i] = static_cast<std::uint8_t>(bits >> (8 * i));
    }
    std::uint32_t words[16]{};
    for (int i = 0; i < 16; ++i)
    {
        for (int j = 0; j < 4; ++j)
        {
            words[i] |= std::uint32_t(block[4 * i + j]) << (8 * j);
        }
    }
    std::uint32_t a = 0x67452301U, b = 0xefcdab89U, c = 0x98badcfeU, d = 0x10325476U;
    const std::uint32_t initial[4] = {a, b, c, d};
    for (std::uint32_t i = 0; i < 64; ++i)
    {
        std::uint32_t f, g;
        if (i < 16)
        {
            f = (b & c) | ((~b) & d);
            g = i;
        }
        else if (i < 32)
        {
            f = (d & b) | ((~d) & c);
            g = (5 * i + 1) & 15;
        }
        else if (i < 48)
        {
            f = b ^ c ^ d;
            g = (3 * i + 5) & 15;
        }
        else
        {
            f = c ^ (b | (~d));
            g = (7 * i) & 15;
        }
        auto prior = d;
        d = c;
        c = b;
        auto value = a + f + k[i] + words[g];
        b += (value << shifts[i]) | (value >> (32 - shifts[i]));
        a = prior;
    }
    const std::uint32_t digest[4] = {initial[0] + a, initial[1] + b, initial[2] + c, initial[3] + d};
    for (int i = 0; i < 4; ++i)
    {
        for (int j = 0; j < 4; ++j)
        {
            uuid[4 * i + j] = static_cast<std::uint8_t>(digest[i] >> (8 * j));
        }
    }
    uuid[6] = (uuid[6] & 0x0f) | 0x30;
    uuid[8] = (uuid[8] & 0x3f) | 0x80;
    return true;
}
} // namespace craft
