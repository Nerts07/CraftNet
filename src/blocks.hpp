#pragma once
#include "world.hpp"

namespace craft::blocks
{
// Vanilla Java 26.3 default block-state IDs from the local registry report.
constexpr BlockState Air = 0, Stone = 1, Grass = 9, Dirt = 10, Cobble = 14, Planks = 15;
constexpr BlockState Bedrock = 88, Water = 89, Sand = 121, Gravel = 127, IronOre = 134, CoalOre = 136;
constexpr BlockState OakLog = 140, BirchLog = 146, OakLeaves = 294, BirchLeaves = 350;
constexpr BlockState Sandstone = 677, CraftingTable = 6866, Snow = 8598, Deepslate = 33774;
} // namespace craft::blocks
