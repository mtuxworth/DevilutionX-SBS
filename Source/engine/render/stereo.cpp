#include "engine/render/stereo.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>
#include <thread>
#include <vector>

namespace devilution::stereo {

namespace {

constexpr int16_t EmptyDepth = std::numeric_limits<int16_t>::min();

std::vector<int16_t> DepthBuffer;
std::vector<uint8_t> TileMask;

struct OverlayRegion {
	int x;
	int y;
	int width;
	int height;
	int16_t depth;
};
/** Areas of overlay pixels that belong to something in the world, registered after the world is drawn. */
std::vector<OverlayRegion> OverlayRegions;
/** The frame just after labels were drawn; later drawing (panels, cursor) differs from it and stays flat. */
std::vector<uint8_t> OverlaySnapshot;
bool OverlaySnapshotValid;

struct CursorState {
	int x = 0;
	int y = 0;
	int width = 0;
	int height = 0;
	int hotspotX = 0;
	int hotspotY = 0;
	/** What the frame held under the cursor before it was drawn. */
	std::vector<uint8_t> behind;
};
CursorState Cursor;

template <typename Fn>
void ParallelFor(int count, Fn &&fn)
{
	const int threadCount = static_cast<int>(std::clamp(std::thread::hardware_concurrency(), 1U, 8U));
	const int perThread = (count + threadCount - 1) / threadCount;
	std::vector<std::thread> workers;
	for (int t = 1; t < threadCount; ++t) {
		const int begin = t * perThread;
		if (begin >= count)
			break;
		workers.emplace_back(fn, begin, std::min(begin + perThread, count));
	}
	fn(0, std::min(perThread, count));
	for (std::thread &worker : workers)
		worker.join();
}
std::vector<uint8_t> WorldSnapshot;
int WorldPitch;
int WorldWidth;
int WorldHeight;
bool WorldValid;

/**
 * Eyes are warped on a grid this many times finer than a game pixel, then averaged back down.
 * Whole-pixel shifts make the smoothly sloping floor jump a full pixel of depth every few rows,
 * which is seen as ripples across the ground.
 */
constexpr int SubPixels = 4;

struct RowScratch {
	std::vector<uint8_t> color;
	std::vector<int16_t> depth;
	/** Overlay pixels already placed at a world depth in this row. */
	std::vector<uint8_t> handled;
};

void UpscaleDepth(int offsetX, int width, int height)
{
	// In place, iterating backwards so every source value is read before it is overwritten.
	int16_t *depth = DepthBuffer.data();
	uint8_t *mask = TileMask.data();
	for (int y = height - 1; y >= 0; --y) {
		const ptrdiff_t srcRow = static_cast<ptrdiff_t>(y / 2) * WorldPitch;
		const ptrdiff_t dstRow = static_cast<ptrdiff_t>(y) * WorldPitch + offsetX;
		for (int x = width - 1; x >= 0; --x) {
			depth[dstRow + x] = static_cast<int16_t>(depth[srcRow + x / 2] * 2);
			mask[dstRow + x] = mask[srcRow + x / 2];
		}
	}
}

/** Horizontal and vertical reach of the wall/roof depth smoothing, in game pixels (about one tile). */
constexpr int SmoothRadiusX = 32;
constexpr int SmoothRadiusY = 16;

/**
 * Replaces the depth of every wall/roof pixel with the average depth of the wall/roof pixels around it.
 * Floor, sprites and items are neither changed nor included. Box filter using running sums, so the cost
 * doesn't depend on the radius.
 */
void SmoothTileDepth()
{
	const int width = WorldWidth;
	const int height = WorldHeight;
	const int pitch = WorldPitch;
	int16_t *depth = DepthBuffer.data();
	const uint8_t *mask = TileMask.data();

	// Horizontal pass: per pixel, the sum and count of masked depths in the row window. Rows split across cores.
	static std::vector<int32_t> rowSum;
	static std::vector<int16_t> rowCount;
	rowSum.resize(static_cast<size_t>(width) * height);
	rowCount.resize(static_cast<size_t>(width) * height);
	ParallelFor(height, [&](int firstRow, int endRow) {
		for (int y = firstRow; y < endRow; ++y) {
			const int16_t *z = depth + static_cast<ptrdiff_t>(y) * pitch;
			const uint8_t *m = mask + static_cast<ptrdiff_t>(y) * pitch;
			int32_t *sumOut = rowSum.data() + static_cast<ptrdiff_t>(y) * width;
			int16_t *countOut = rowCount.data() + static_cast<ptrdiff_t>(y) * width;
			int32_t sum = 0;
			int count = 0;
			for (int x = 0; x < std::min(SmoothRadiusX, width); ++x) {
				if (m[x] != 0) {
					sum += z[x];
					++count;
				}
			}
			for (int x = 0; x < width; ++x) {
				const int add = x + SmoothRadiusX;
				if (add < width && m[add] != 0) {
					sum += z[add];
					++count;
				}
				const int remove = x - SmoothRadiusX - 1;
				if (remove >= 0 && m[remove] != 0) {
					sum -= z[remove];
					--count;
				}
				sumOut[x] = sum;
				countOut[x] = static_cast<int16_t>(count);
			}
		}
	});

	// Vertical pass over the row results, sliding a window of rows down each column. Columns split across cores.
	ParallelFor(width, [&](int firstColumn, int endColumn) {
		const int columns = endColumn - firstColumn;
		std::vector<int32_t> columnSum(columns, 0);
		std::vector<int32_t> columnCount(columns, 0);
		auto addRow = [&](int row, int sign) {
			const int32_t *s = rowSum.data() + static_cast<ptrdiff_t>(row) * width + firstColumn;
			const int16_t *c = rowCount.data() + static_cast<ptrdiff_t>(row) * width + firstColumn;
			for (int i = 0; i < columns; ++i) {
				columnSum[i] += sign * s[i];
				columnCount[i] += sign * c[i];
			}
		};
		for (int y = 0; y < std::min(SmoothRadiusY, height); ++y)
			addRow(y, 1);
		for (int y = 0; y < height; ++y) {
			if (y + SmoothRadiusY < height)
				addRow(y + SmoothRadiusY, 1);
			if (y - SmoothRadiusY - 1 >= 0)
				addRow(y - SmoothRadiusY - 1, -1);
			int16_t *z = depth + static_cast<ptrdiff_t>(y) * pitch + firstColumn;
			const uint8_t *m = mask + static_cast<ptrdiff_t>(y) * pitch + firstColumn;
			for (int i = 0; i < columns; ++i) {
				if (m[i] != 0 && columnCount[i] > 0)
					z[i] = static_cast<int16_t>(columnSum[i] / columnCount[i]);
			}
		}
	});
}

uint32_t Pack(const RgbFrame &frame, unsigned r, unsigned g, unsigned b)
{
	return (r << frame.rShift) | (g << frame.gShift) | (b << frame.bShift) | frame.alphaMask;
}

uint32_t PackIndex(const RgbFrame &frame, uint8_t index)
{
	const uint8_t *rgb = frame.palette + 3 * index;
	return Pack(frame, rgb[0], rgb[1], rgb[2]);
}

/**
 * Forward-warps one row of the world into one eye on the sub-pixel grid, fills disocclusion holes from the
 * background, then averages each game pixel's sub-pixels into a blended colour.
 * @param shiftPerUnit Shift in sub-pixels per unit of nearness, 16.16 fixed point.
 */
void WarpRow(const uint8_t *world, const int16_t *depth, int width, int16_t zeroPlane, int32_t shiftPerUnit, int sign,
    RowScratch &scratch, const RgbFrame &frame, uint32_t *out)
{
	const int subWidth = width * SubPixels;
	scratch.color.resize(subWidth);
	scratch.depth.assign(subWidth, EmptyDepth);
	uint8_t *color = scratch.color.data();
	int16_t *zrow = scratch.depth.data();

	for (int x = 0; x < width; ++x) {
		const int16_t z = depth[x];
		const int32_t shift = ((static_cast<int32_t>(z) - zeroPlane) * shiftPerUnit + (1 << 15)) >> 16;
		const int start = x * SubPixels + sign * shift;
		const int begin = std::max(start, 0);
		const int end = std::min(start + SubPixels, subWidth);
		for (int i = begin; i < end; ++i) {
			if (z > zrow[i]) {
				zrow[i] = z;
				color[i] = world[x];
			}
		}
	}

	// Fill holes with whichever neighbour is farther away, which is the surface that was revealed.
	int i = 0;
	while (i < subWidth) {
		if (zrow[i] != EmptyDepth) {
			++i;
			continue;
		}
		const int start = i;
		while (i < subWidth && zrow[i] == EmptyDepth)
			++i;
		const int left = start - 1;
		const int right = i;
		uint8_t fill = 0;
		if (left >= 0 && right < subWidth) {
			fill = zrow[left] <= zrow[right] ? color[left] : color[right];
		} else if (left >= 0) {
			fill = color[left];
		} else if (right < subWidth) {
			fill = color[right];
		}
		std::fill(color + start, color + i, fill);
	}

	for (int x = 0; x < width; ++x) {
		unsigned r = 0;
		unsigned g = 0;
		unsigned b = 0;
		for (int s = 0; s < SubPixels; ++s) {
			const uint8_t *rgb = frame.palette + 3 * color[x * SubPixels + s];
			r += rgb[0];
			g += rgb[1];
			b += rgb[2];
		}
		out[x] = Pack(frame, r / SubPixels, g / SubPixels, b / SubPixels);
	}
}

} // namespace

void BeginWorld(uint8_t *base, int pitch, int width, int height)
{
	WorldPitch = pitch;
	WorldWidth = width;
	WorldHeight = height;
	DepthBuffer.resize(static_cast<size_t>(pitch) * height);
	TileMask.assign(static_cast<size_t>(pitch) * height, 0);
	OverlayRegions.clear();
	OverlaySnapshotValid = false;
	for (int y = 0; y < height; ++y) {
		std::fill_n(DepthBuffer.data() + static_cast<ptrdiff_t>(y) * pitch, width, Nearness(y, FloorFootY));
	}

	Writer.base = base;
	Writer.pitch = pitch;
	Writer.height = height;
	Writer.depth = DepthBuffer.data();
	Writer.isTile = TileMask.data();
	Writer.foot = FootLine {};
	Writer.active = true;
}

void EndWorld(int zoomOffsetX, int zoomWidth)
{
	Writer.active = false;
	if (zoomWidth > 0) {
		UpscaleDepth(zoomOffsetX, zoomWidth, WorldHeight);
	}
	if (Settings.smoothTiles) {
		SmoothTileDepth();
	}

	WorldSnapshot.resize(static_cast<size_t>(WorldPitch) * WorldHeight);
	std::memcpy(WorldSnapshot.data(), Writer.base, WorldSnapshot.size());
	WorldValid = true;
}

void InvalidateWorld()
{
	WorldValid = false;
}

void Compose(const uint8_t *frame, int pitch, int width, int height, const RgbFrame &out)
{
	const bool hasWorld = WorldValid && WorldPitch == pitch && WorldWidth == width && WorldHeight <= height;
	const int worldHeight = hasWorld ? WorldHeight : 0;

	// Fixed-point 16.16 shift per eye (half the total disparity), in sub-pixels, for one unit of nearness.
	const float pixelsPerUnit = worldHeight > 0 ? Settings.strength * width / (3.0F * worldHeight) : 0.0F;
	const auto shiftPerUnit = static_cast<int32_t>(pixelsPerUnit * 0.5F * SubPixels * 65536.0F);
	const auto zeroPlane = static_cast<int16_t>(3.0F * Settings.convergence * worldHeight);
	const int leftSign = Settings.swapEyes ? -1 : 1;

	// Labels and the cursor are flat, so they move by whole pixels; that's fine at a single depth.
	const auto wholePixelShift = [&](int16_t z) {
		const int32_t subShift = ((static_cast<int32_t>(z) - zeroPlane) * shiftPerUnit + (1 << 15)) >> 16;
		return (subShift >= 0 ? subShift + SubPixels / 2 : subShift - SubPixels / 2) / SubPixels;
	};

	// The cursor sits at the depth of whatever is under its hotspot. Over the HUD it's left on the screen plane.
	bool cursorHasDepth = false;
	int cursorShift = 0;
	if (hasWorld && Cursor.width > 0 && Cursor.height > 0) {
		const int hx = Cursor.hotspotX;
		const int hy = Cursor.hotspotY;
		if (hx >= 0 && hx < width && hy >= 0 && hy < worldHeight) {
			const ptrdiff_t i = static_cast<ptrdiff_t>(hy) * pitch + hx;
			uint8_t under = frame[i];
			if (hx >= Cursor.x && hx < Cursor.x + Cursor.width && hy >= Cursor.y && hy < Cursor.y + Cursor.height)
				under = Cursor.behind[static_cast<size_t>(hy - Cursor.y) * Cursor.width + (hx - Cursor.x)];
			if (under == WorldSnapshot[i]) {
				cursorHasDepth = true;
				cursorShift = wholePixelShift(DepthBuffer[i]);
			} else if (OverlaySnapshotValid && under == OverlaySnapshot[i]) {
				for (const OverlayRegion &region : OverlayRegions) {
					if (hx >= region.x && hx < region.x + region.width && hy >= region.y && hy < region.y + region.height) {
						cursorHasDepth = true;
						cursorShift = wholePixelShift(region.depth);
						break;
					}
				}
			}
		}
	}
	const int cursorBegin = std::max(Cursor.x, 0);
	const int cursorEnd = std::min(Cursor.x + Cursor.width, width);

	const auto composeRows = [&](int firstRow, int endRow) {
		RowScratch scratch;
		for (int y = firstRow; y < endRow; ++y) {
			const uint8_t *src = frame + static_cast<ptrdiff_t>(y) * pitch;
			uint32_t *leftEye = out.pixels + static_cast<ptrdiff_t>(y) * out.pitch;
			uint32_t *rightEye = leftEye + width;
			const uint8_t *behindCursor = cursorHasDepth && y >= Cursor.y && y < Cursor.y + Cursor.height
			    ? Cursor.behind.data() + static_cast<size_t>(y - Cursor.y) * Cursor.width
			    : nullptr;
			const auto drawCursor = [&]() {
				for (int x = cursorBegin; x < cursorEnd; ++x) {
					if (src[x] == behindCursor[x - Cursor.x])
						continue;
					const uint32_t color = PackIndex(out, src[x]);
					const int leftX = x + leftSign * cursorShift;
					const int rightX = x - leftSign * cursorShift;
					if (leftX >= 0 && leftX < width)
						leftEye[leftX] = color;
					if (rightX >= 0 && rightX < width)
						rightEye[rightX] = color;
				}
			};

			if (y >= worldHeight) {
				for (int x = 0; x < width; ++x)
					leftEye[x] = rightEye[x] = PackIndex(out, src[x]);
				if (behindCursor != nullptr) {
					for (int x = cursorBegin; x < cursorEnd; ++x) {
						if (src[x] != behindCursor[x - Cursor.x])
							leftEye[x] = rightEye[x] = PackIndex(out, behindCursor[x - Cursor.x]);
					}
					drawCursor();
				}
				continue;
			}

			const uint8_t *world = WorldSnapshot.data() + static_cast<ptrdiff_t>(y) * pitch;
			const int16_t *depth = DepthBuffer.data() + static_cast<ptrdiff_t>(y) * pitch;
			// Nearer pixels appear further right to the left eye and further left to the right eye.
			WarpRow(world, depth, width, zeroPlane, shiftPerUnit, leftSign, scratch, out, leftEye);
			WarpRow(world, depth, width, zeroPlane, shiftPerUnit, -leftSign, scratch, out, rightEye);

			scratch.handled.assign(width, 0);
			// Cursor pixels are drawn last at the cursor's depth; at their own spot, show what was behind them.
			if (behindCursor != nullptr) {
				for (int x = cursorBegin; x < cursorEnd; ++x) {
					if (src[x] == behindCursor[x - Cursor.x])
						continue;
					scratch.handled[x] = 1;
					if (behindCursor[x - Cursor.x] != world[x])
						leftEye[x] = rightEye[x] = PackIndex(out, behindCursor[x - Cursor.x]);
				}
			}

			// Labels and damage numbers sit at the depth of what they belong to.
			if (OverlaySnapshotValid) {
				const uint8_t *overlay = OverlaySnapshot.data() + static_cast<ptrdiff_t>(y) * pitch;
				for (const OverlayRegion &region : OverlayRegions) {
					if (y < region.y || y >= region.y + region.height)
						continue;
					const int shift = wholePixelShift(region.depth);
					const int begin = std::max(region.x, 0);
					const int end = std::min(region.x + region.width, width);
					for (int x = begin; x < end; ++x) {
						if (scratch.handled[x] != 0 || src[x] == world[x] || src[x] != overlay[x])
							continue;
						scratch.handled[x] = 1;
						const uint32_t color = PackIndex(out, src[x]);
						const int leftX = x + leftSign * shift;
						const int rightX = x - leftSign * shift;
						if (leftX >= 0 && leftX < width)
							leftEye[leftX] = color;
						if (rightX >= 0 && rightX < width)
							rightEye[rightX] = color;
					}
				}
			}

			// Everything else drawn on top of the world (HUD, panels) stays on the screen plane.
			for (int x = 0; x < width; ++x) {
				if (src[x] != world[x] && scratch.handled[x] == 0)
					leftEye[x] = rightEye[x] = PackIndex(out, src[x]);
			}
			if (behindCursor != nullptr)
				drawCursor();
		}
	};

	// Rows are independent, so split them across cores.
	ParallelFor(height, composeRows);

	ShiftRightEyeVertically(out.pixels, out.pitch, width, height, Settings.verticalOffset);
}

void ShiftRightEyeVertically(uint32_t *out, int outPitch, int width, int height, int offset)
{
	offset = std::clamp(offset, -height, height);
	if (offset == 0)
		return;
	const size_t rowBytes = static_cast<size_t>(width) * sizeof(uint32_t);
	auto rightEyeRow = [&](int y) { return out + static_cast<ptrdiff_t>(y) * outPitch + width; };
	if (offset > 0) {
		for (int y = height - 1; y >= offset; --y)
			std::memcpy(rightEyeRow(y), rightEyeRow(y - offset), rowBytes);
		for (int y = 0; y < offset; ++y)
			std::memset(rightEyeRow(y), 0, rowBytes);
	} else {
		for (int y = 0; y < height + offset; ++y)
			std::memcpy(rightEyeRow(y), rightEyeRow(y - offset), rowBytes);
		for (int y = height + offset; y < height; ++y)
			std::memset(rightEyeRow(y), 0, rowBytes);
	}
}

bool SaveDepthImage(const char *path)
{
	if (!WorldValid || WorldHeight <= 0)
		return false;

	int16_t minZ = std::numeric_limits<int16_t>::max();
	int16_t maxZ = std::numeric_limits<int16_t>::min();
	for (int y = 0; y < WorldHeight; ++y) {
		const int16_t *row = DepthBuffer.data() + static_cast<ptrdiff_t>(y) * WorldPitch;
		for (int x = 0; x < WorldWidth; ++x) {
			minZ = std::min(minZ, row[x]);
			maxZ = std::max(maxZ, row[x]);
		}
	}
	const int range = std::max(1, maxZ - minZ);

	FILE *file = std::fopen(path, "wb");
	if (file == nullptr)
		return false;
	const int rowBytes = (WorldWidth * 3 + 3) & ~3;
	const uint32_t imageSize = static_cast<uint32_t>(rowBytes) * WorldHeight;
	const uint32_t fileSize = 54 + imageSize;
	uint8_t header[54] = { 'B', 'M' };
	auto put32 = [&header](int offset, uint32_t value) {
		for (int i = 0; i < 4; ++i)
			header[offset + i] = static_cast<uint8_t>(value >> (8 * i));
	};
	put32(2, fileSize);
	put32(10, 54);
	put32(14, 40);
	put32(18, static_cast<uint32_t>(WorldWidth));
	put32(22, static_cast<uint32_t>(WorldHeight));
	header[26] = 1;
	header[28] = 24;
	put32(34, imageSize);
	std::fwrite(header, 1, sizeof(header), file);

	std::vector<uint8_t> line(rowBytes, 0);
	for (int y = WorldHeight - 1; y >= 0; --y) {
		const int16_t *row = DepthBuffer.data() + static_cast<ptrdiff_t>(y) * WorldPitch;
		for (int x = 0; x < WorldWidth; ++x) {
			const auto grey = static_cast<uint8_t>((row[x] - minZ) * 255 / range);
			line[x * 3] = line[x * 3 + 1] = line[x * 3 + 2] = grey;
		}
		std::fwrite(line.data(), 1, line.size(), file);
	}
	std::fclose(file);
	return true;
}

void AddOverlayDepth(int x, int y, int width, int height, int footY)
{
	if (!Settings.enabled || Writer.base == nullptr || width <= 0 || height <= 0)
		return;
	OverlayRegions.push_back(OverlayRegion { x, y, width, height, Nearness(footY, FloorFootY) });
}

void SetCursor(int x, int y, int width, int height, const uint8_t *behind, int hotspotX, int hotspotY)
{
	if (!Settings.enabled || width <= 0 || height <= 0 || behind == nullptr) {
		Cursor.width = Cursor.height = 0;
		return;
	}
	Cursor.x = x;
	Cursor.y = y;
	Cursor.width = width;
	Cursor.height = height;
	Cursor.hotspotX = hotspotX;
	Cursor.hotspotY = hotspotY;
	Cursor.behind.assign(behind, behind + static_cast<size_t>(width) * height);
}

void SnapshotOverlays()
{
	if (!Settings.enabled || !WorldValid || OverlayRegions.empty())
		return;
	OverlaySnapshot.resize(static_cast<size_t>(WorldPitch) * WorldHeight);
	std::memcpy(OverlaySnapshot.data(), Writer.base, OverlaySnapshot.size());
	OverlaySnapshotValid = true;
}

} // namespace devilution::stereo
