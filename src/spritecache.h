/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file spritecache.h Functions to cache sprites in memory. */

#ifndef SPRITECACHE_H
#define SPRITECACHE_H

#include "gfx_type.h"
#include "spritecache_type.h"
#include "spriteloader/spriteloader.hpp"
#include "zoom_func.h"

extern uint _sprite_cache_size;

/** SpriteAllocator that allocates memory via a unique_ptr array. */
class UniquePtrSpriteAllocator : public SpriteAllocator {
public:
	std::unique_ptr<std::byte[]> data;
	size_t size;
protected:
	void *AllocatePtr(size_t size) override;
};

void *GetRawSprite(SpriteID sprite, SpriteType type, SpriteAllocator *allocator = nullptr, SpriteEncoder *encoder = nullptr);
bool SpriteExists(SpriteID sprite);

SpriteType GetSpriteType(SpriteID sprite);
SpriteFile *GetOriginFile(SpriteID sprite);
uint32_t GetSpriteLocalID(SpriteID sprite);
uint GetSpriteCountForFile(const std::string &filename, SpriteID begin, SpriteID end);
SpriteID GetMaxSpriteID();


inline const Sprite *GetSprite(SpriteID sprite, SpriteType type)
{
	assert(type != SpriteType::Recolour);
	return (Sprite*)GetRawSprite(sprite, type);
}

inline const uint8_t *GetNonSprite(SpriteID sprite, SpriteType type)
{
	assert(type == SpriteType::Recolour);
	return (uint8_t*)GetRawSprite(sprite, type);
}

/**
 * Where a horizontally mirrored sprite's leftmost column ends up.
 *
 * A sprite is mirrored about its own tile's vertical screen axis, so that a full-tile
 * sprite lands on exactly the columns its unmirrored neighbours leave for it. A world
 * tile spans screen columns -31..32 around its origin, whose midpoint sits half a pixel
 * right of column 0; reflecting column x to (1 - x) is that mirror. Reflecting the whole
 * box [x_offs, x_offs + width - 1] therefore puts its new left edge at
 * 1 - (x_offs + width - 1), which is what this returns.
 *
 * @param width Sprite width, in ZoomLevel::Min units.
 * @param x_offs Sprite's leftmost column, in ZoomLevel::Min units.
 * @return The mirrored sprite's leftmost column, in the same units.
 */
inline int MirroredSpriteXOffset(int width, int x_offs)
{
	/* Twice the mirror axis, which sits between two columns and so is a half-integer. */
	const int axis_x2 = ScaleByZoom(2, ZoomLevel::Normal);
	return axis_x2 - width - x_offs;
}

void SetupMirroredSprites();

void GfxInitSpriteMem();
void GfxClearSpriteCache();
void GfxClearFontSpriteCache();
void IncreaseSpriteLRU();

SpriteFile &OpenCachedSpriteFile(const std::string &filename, Subdirectory subdir, bool palette_remap);
std::span<const std::unique_ptr<SpriteFile>> GetCachedSpriteFiles();

void ReadGRFSpriteOffsets(SpriteFile &file);
size_t GetGRFSpriteOffset(uint32_t id);
bool LoadNextSprite(SpriteID load_index, SpriteFile &file, uint file_sprite_id);
bool SkipSpriteData(SpriteFile &file, uint8_t type, uint16_t num);
void DupSprite(SpriteID old_spr, SpriteID new_spr);

#endif /* SPRITECACHE_H */
