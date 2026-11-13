#pragma once
#include "wire.hpp"
#include <string>

namespace craft
{
// Decodes a chat component into plain text.
bool readChatText(wire::Reader &reader, std::string &output);
bool readRichChatText(wire::Reader &reader, std::string &output);
// Decodes a play-state player chat packet into display text.
bool readPlayerChat(wire::Reader &reader, std::string &output);
} // namespace craft
