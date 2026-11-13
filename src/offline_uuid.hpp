#pragma once
#include <array>
#include <cstdint>
#include <string>

namespace craft
{
// Builds the offline-mode UUID for a player name.
bool offlineUuid(const std::string &name, std::array<std::uint8_t, 16> &uuid);
} // namespace craft
