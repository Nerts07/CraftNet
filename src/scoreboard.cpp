#include "session.hpp"
#include "chat.hpp"
#include <algorithm>

namespace craft
{
bool Session::ScoreFormat::read(wire::Reader &r)
{
    std::uint8_t present;
    std::int32_t kind;
    if (!r.byte(present) || present > 1)
    {
        return false;
    }
    if (!present)
    {
        return true;
    }
    if (!r.var(kind) || kind < 0 || kind > 2 || (kind && !readRichChatText(r, text)))
    {
        return false;
    }
    type = kind;
    if (type == 1 && text.size() >= 3 && text.compare(text.size() - 3, 3, "\xc2\xa7r") == 0)
    {
        text.resize(text.size() - 3);
    }
    return true;
}

void Session::readScoreboard(int id, wire::Reader &r)
{
    if (id == 100)
    {
        std::int32_t slot;
        std::string name;
        if (!r.var(slot) || slot < 0 || slot >= static_cast<int>(scoreSlots_.size()) || !r.str(name, 128) ||
            r.remaining())
        {
            return;
        }
        scoreSlots_[slot] = std::move(name);
    }
    else if (id == 108)
    {
        ScoreObjective objective;
        std::uint8_t mode;
        std::int32_t renderType;
        if (!r.str(objective.name, 128) || !r.byte(mode) || mode > 2 ||
            (mode != 1 && (!readRichChatText(r, objective.title) || !r.var(renderType) || !objective.format.read(r))) ||
            r.remaining())
        {
            return;
        }
        const auto found = std::find_if(objectives_.begin(), objectives_.end(), [&](const ScoreObjective &current)
                                        { return current.name == objective.name; });
        if (mode == 1)
        {
            if (found != objectives_.end())
            {
                objectives_.erase(found);
            }
            for (auto &slot : scoreSlots_)
            {
                if (slot == objective.name)
                {
                    slot.clear();
                }
            }
            scores_.erase(std::remove_if(scores_.begin(), scores_.end(), [&](const ScoreEntry &entry)
                                        { return entry.objective == objective.name; }), scores_.end());
        }
        else if (found != objectives_.end())
        {
            *found = std::move(objective);
        }
        else if (objectives_.size() < 64)
        {
            objectives_.push_back(std::move(objective));
        }
    }
    else if (id == 112)
    {
        ScoreEntry score;
        std::uint8_t custom;
        if (!r.str(score.owner, 256) || !r.str(score.objective, 128) || !r.var(score.value) || !r.byte(custom) ||
            custom > 1 || (custom && !readRichChatText(r, score.display)) || !score.format.read(r) || r.remaining())
        {
            return;
        }
        score.customDisplay = custom != 0;
        const auto found = std::find_if(scores_.begin(), scores_.end(), [&](const ScoreEntry &entry)
                                        { return entry.owner == score.owner && entry.objective == score.objective; });
        if (found != scores_.end())
        {
            *found = std::move(score);
        }
        else if (scores_.size() < 1024)
        {
            scores_.push_back(std::move(score));
        }
    }
    else
    {
        std::string owner, name;
        std::uint8_t objective;
        if (!r.str(owner, 256) || !r.byte(objective) || objective > 1 ||
            (objective && !r.str(name, 128)) || r.remaining())
        {
            return;
        }
        scores_.erase(std::remove_if(scores_.begin(), scores_.end(), [&](const ScoreEntry &entry)
                                    { return entry.owner == owner && (!objective || entry.objective == name); }),
                      scores_.end());
    }
    sidebarDirty_ = true;
}

void Session::refreshSidebar()
{
    sidebarDirty_ = false;
    sidebar_.visible = false;
    sidebar_.count = 0;
    const auto teamFor = [&](const std::string &name) -> const Team *
    {
        for (const auto &member : members_)
        {
            if (member.team >= 0 && member.account == name && teams_[member.team].used)
            {
                return &teams_[member.team];
            }
        }
        return nullptr;
    };
    int slot = 1;
    const auto *team = teamFor(username_);
    if (team && team->color >= 0 && team->color < 16 && !scoreSlots_[3 + team->color].empty())
    {
        slot = 3 + team->color;
    }
    const auto &name = scoreSlots_[slot];
    if (name.empty())
    {
        return;
    }
    const auto objective = std::find_if(objectives_.begin(), objectives_.end(),
                                        [&](const ScoreObjective &entry) { return entry.name == name; });
    if (objective == objectives_.end())
    {
        return;
    }

    std::array<const ScoreEntry *, 1024> ordered;
    auto end = ordered.begin();
    for (const auto &score : scores_)
    {
        if (score.objective == name && !score.owner.empty() && score.owner.front() != '#')
        {
            *end++ = &score;
        }
    }
    const auto visibleEnd = ordered.begin() + std::min(end - ordered.begin(),
                                                       static_cast<std::ptrdiff_t>(sidebar_.rows.size()));
    std::partial_sort(ordered.begin(), visibleEnd, end, [](const ScoreEntry *a, const ScoreEntry *b)
                      { return a->value != b->value ? a->value > b->value : a->owner < b->owner; });
    sidebar_.visible = true;
    sidebar_.title = objective->title;
    for (auto it = ordered.begin(); it != visibleEnd; ++it)
    {
        const auto &entry = **it;
        auto &row = sidebar_.rows[sidebar_.count++];
        const auto *entryTeam = teamFor(entry.owner);
        row.label = entry.customDisplay ? entry.display :
                    entryTeam ? entryTeam->prefix + entry.owner + entryTeam->suffix : entry.owner;
        const auto &format = entry.format.type >= 0 ? entry.format : objective->format;
        if (format.type == 0)
        {
            row.value.clear();
        }
        else if (format.type == 2)
        {
            row.value = format.text;
        }
        else
        {
            row.value = (format.type == 1 ? format.text : "\xc2\xa7" "c") + std::to_string(entry.value);
        }
    }
}
} // namespace craft
