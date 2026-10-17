/**
 * @file stereo.hpp
 *
 * Side-by-side stereoscopic 3D output.
 *
 * While the dungeon is drawn, every pixel written by the tile and sprite
 * blitters also records a depth value derived from the screen-space "foot"
 * (ground contact line) of the element being drawn. After the frame is
 * complete, each eye is produced by shifting world pixels horizontally in
 * proportion to their depth (depth-image-based rendering). Everything drawn
 * after the world (HUD, panels, text, cursor) is detected by diffing against a
 * snapshot of the world and is kept at zero disparity (the screen plane).
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "utils/attributes.h"

namespace devilution::stereo {

struct StereoSettings {
	bool enabled = false;
	/** Total disparity across the floor of the viewport, as a fraction of one eye's width. */
	float strength = 0.02F;
	/** Height of the zero-parallax plane, as a fraction of the viewport height (0 = top, 1 = bottom). */
	float convergence = 0.8F;
	bool swapEyes = false;
	/** Display index to use, or -1 to pick the first full side-by-side display. */
	int display = -1;
	/** Moves the right eye's image down (positive) or up (negative) by this many game pixels, to correct headset alignment. */
	int verticalOffset = 0;
	/** Diagnostic: one foot row per tile at its centre, and items/corpses standing, as in the first version. */
	bool legacyDepth = false;
	/**
	 * Blend depth across neighbouring wall and roof pixels. Large roofs are drawn as patches from a grid of
	 * tiles, each with its own foot line, which otherwise makes individual patches stand out.
	 */
	bool smoothTiles = true;
	/** Set by command-line flags, so the saved settings don't replace those values for this run. */
	bool strengthFromCommandLine = false;
	bool convergenceFromCommandLine = false;
	bool swapEyesFromCommandLine = false;
};

inline StereoSettings Settings;

/** Set to save the next composed side-by-side frame as sbs_frame.bmp next to the executable. */
inline bool DumpNextFrame = false;

/** Foot value that makes every pixel lie on the floor plane. */
constexpr int FloorFootY = -32768;

/**
 * Where the element being drawn meets the ground, in screen rows.
 *
 * Sprites stand on a single row. Dungeon tiles stand on their diamond's two back edges
 * (a "^" from the left corner up to the top corner and down to the right corner), so
 * walls running along a line of tiles get one continuous foot line instead of a step per tile.
 */
struct FootLine {
	/** The foot row, or for back edges the row of the diamond's top corner. */
	int y = FloorFootY;
	/** Screen column of the diamond's top corner (back edges only). */
	int peakX = 0;
	bool backEdges = false;
};

/** Half the width of a dungeon tile diamond; its back edges rise one row every two columns. */
constexpr int TileHalfWidth = 32;

struct DepthWriter {
	const uint8_t *base = nullptr;
	int pitch = 0;
	int height = 0;
	int16_t *depth = nullptr;
	/** 1 where the pixel is part of a wall or roof (a tile pixel above its foot line), same layout as depth. */
	uint8_t *isTile = nullptr;
	FootLine foot;
	bool active = false;
};

inline DepthWriter Writer;

/**
 * Nearness of a pixel (larger = closer to the camera), scaled by 3.
 *
 * For Diablo's ~30 degree camera, a point on the ground at screen row y has
 * nearness ~y, and a point on a vertical surface standing on screen row footY
 * has nearness ~footY + (footY - y) / 3. Pixels below the foot line are
 * treated as floor.
 */
DVL_ALWAYS_INLINE int16_t Nearness(int y, int footY)
{
	return static_cast<int16_t>(y > footY ? 3 * y : 4 * footY - y);
}

DVL_ALWAYS_INLINE void MarkDepthSlow(const uint8_t *dst, unsigned n)
{
	const ptrdiff_t offset = dst - Writer.base;
	if (offset < 0)
		return;
	const int y = static_cast<int>(offset / Writer.pitch);
	if (y >= Writer.height)
		return;
	int16_t *out = Writer.depth + offset;
	const FootLine &foot = Writer.foot;
	if (!foot.backEdges) {
		const int16_t z = Nearness(y, foot.y);
		for (unsigned i = 0; i < n; ++i)
			out[i] = z;
		std::memset(Writer.isTile + offset, 0, n);
		return;
	}
	const int x = static_cast<int>(offset - static_cast<ptrdiff_t>(y) * Writer.pitch);
	uint8_t *isTile = Writer.isTile + offset;
	for (unsigned i = 0; i < n; ++i) {
		int distance = x + static_cast<int>(i) - foot.peakX;
		distance = distance < 0 ? -distance : distance;
		const int footY = foot.y + (distance < TileHalfWidth ? distance : TileHalfWidth) / 2;
		out[i] = Nearness(y, footY);
		isTile[i] = y <= footY ? 1 : 0;
	}
}

/** Called by the low-level blitters for every horizontal run of pixels they write. */
DVL_ALWAYS_INLINE void MarkDepth(const uint8_t *dst, unsigned n)
{
	if (!Writer.active)
		return;
	MarkDepthSlow(dst, n);
}

/** Sets the foot line for subsequent world draws, restoring the previous one on destruction. */
class FootScope {
public:
	/** Stand on a single screen row (sprites), or FloorFootY to lie flat. */
	explicit FootScope(int footY)
	    : previous_(Writer.foot)
	{
		Writer.foot = FootLine { footY, 0, false };
	}
	/** Stand on the back edges of the tile diamond whose top corner is at (peakX, peakY). */
	FootScope(int peakY, int peakX)
	    : previous_(Writer.foot)
	{
		Writer.foot = BackEdges(peakY, peakX);
	}
	static FootLine BackEdges(int peakY, int peakX)
	{
		return Settings.legacyDepth
		    ? FootLine { peakY + TileHalfWidth / 2, 0, false } // the tile centre row
		    : FootLine { peakY, peakX, true };
	}
	/** Lie flat on the floor, except in legacy depth mode where the current foot is kept. */
	static FootLine Flat()
	{
		return Settings.legacyDepth ? Writer.foot : FootLine {};
	}
	explicit FootScope(const FootLine &foot)
	    : previous_(Writer.foot)
	{
		Writer.foot = foot;
	}
	~FootScope()
	{
		Writer.foot = previous_;
	}
	FootScope(const FootScope &) = delete;
	FootScope &operator=(const FootScope &) = delete;

private:
	FootLine previous_;
};

/** Starts depth capture for a world render into the given 8-bit buffer. */
void BeginWorld(uint8_t *base, int pitch, int width, int height);

/**
 * Stops depth capture and snapshots the rendered world.
 * @param zoomOffsetX,zoomWidth When zoomWidth > 0 the world was rendered at half size and
 *        upscaled 2x into [zoomOffsetX, zoomOffsetX + zoomWidth); the depth buffer is upscaled to match.
 */
void EndWorld(int zoomOffsetX, int zoomWidth);

/** Drops the world snapshot so the next compose falls back to a flat image. */
void InvalidateWorld();

/** A 32-bit output frame (2 * width wide) and how to encode colours into it. */
struct RgbFrame {
	uint32_t *pixels;
	/** Row stride in pixels. */
	int pitch;
	int rShift;
	int gShift;
	int bShift;
	uint32_t alphaMask;
	/** The current palette, 256 entries of R, G, B. */
	const uint8_t *palette;
};

/**
 * Builds the full side-by-side frame from the final 8-bit frame, with sub-pixel shifts blended into colour.
 * Without a valid world snapshot, the frame is duplicated into both eyes.
 */
void Compose(const uint8_t *frame, int pitch, int width, int height, const RgbFrame &out);

/** Moves the right half of a side-by-side frame down by offset rows (up if negative), filling with black. */
void ShiftRightEyeVertically(uint32_t *out, int outPitch, int width, int height, int offset);

/** Saves the last world depth buffer as a greyscale BMP (brighter = nearer). Returns false on failure. */
bool SaveDepthImage(const char *path);

/**
 * Gives overlay pixels drawn after the world (item labels, damage numbers) inside the rectangle the depth of
 * the ground at screen row footY, instead of the screen plane. Call after the world is drawn, before Compose.
 */
void AddOverlayDepth(int x, int y, int width, int height, int footY);

/** Call once the overlays registered with AddOverlayDepth are drawn; anything drawn after stays on the screen plane. */
void SnapshotOverlays();

/**
 * Records the software cursor just drawn: its rectangle, the pixels that were behind it (width * height, row by row)
 * and its hotspot. The cursor is shown at the depth of whatever is under the hotspot. A zero-sized rectangle clears it.
 */
void SetCursor(int x, int y, int width, int height, const uint8_t *behind, int hotspotX, int hotspotY);

} // namespace devilution::stereo
