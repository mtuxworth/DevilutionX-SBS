/**
 * @file dx.cpp
 *
 * Implementation of functions setting up the graphics pipeline.
 */
#include "engine/dx.h"

#include <cstdint>

#ifdef USE_SDL3
#include <SDL3/SDL_rect.h>
#include <SDL3/SDL_render.h>
#include <SDL3/SDL_surface.h>
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_video.h>
#else
#include <SDL.h>
#endif

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#include "controls/control_mode.hpp"
#include "controls/plrctrls.h"
#include "engine/render/primitive_render.hpp"
#include "engine/render/stereo.hpp"
#include "headless_mode.hpp"
#include "init.hpp"
#include "options.h"
#include "utils/display.h"
#include "utils/log.hpp"
#include "utils/sdl_wrap.h"
#include "utils/static_vector.hpp"
#include "utils/str_cat.hpp"
#include "utils/ui_fwd.h"

#ifndef USE_SDL1
#include "controls/touch/renderers.h"
#endif

#ifdef __3DS__
#include <3ds.h>
#endif

namespace devilution {

int refreshDelay;
SDL_Renderer *renderer;
#ifndef USE_SDL1
SDLTextureUniquePtr texture;
#endif

/** Currently active palette */
SDLPaletteUniquePtr Palette;

/** 24-bit renderer texture surface */
SDLSurfaceUniquePtr RendererTextureSurface;

/** 8-bit surface that we render to */
SDL_Surface *PalSurface;
namespace {
SDLSurfaceUniquePtr PinnedPalSurface;

#ifndef USE_SDL1
constexpr size_t MaxDirtyRects = 16;
StaticVector<SDL_Rect, MaxDirtyRects> DirtyRects;
bool ForceFullUpdate;
const void *LastPresentedPixels;
#endif
} // namespace

/** Whether we render directly to the screen surface, i.e. `PalSurface == GetOutputSurface()` */
bool RenderDirectlyToOutputSurface;

bool StereoFrameHasWorld;

namespace {

#if !defined(USE_SDL1) && !defined(USE_SDL3)
/** Both eyes in full colour; 32-bit so sub-pixel shifts can be blended. */
SDLSurfaceUniquePtr StereoTextureSurface;
SDLTextureUniquePtr StereoTexture;

void EnsureStereoResources(int width, int height)
{
	if (StereoTextureSurface != nullptr && StereoTextureSurface->w == 2 * width && StereoTextureSurface->h == height)
		return;

	StereoTexture = SDLWrap::CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, 2 * width, height);
	StereoTextureSurface = SDLWrap::CreateRGBSurfaceWithFormat(0, 2 * width, height, 32, SDL_PIXELFORMAT_ARGB8888);
}

/** Textures can be lost when the display mode changes (e.g. glasses toggling 2D/3D); recreate them and skip the frame. */
void RecoverLostTextures()
{
	LogError("Side-by-side 3D: texture update failed ({}), recreating textures", SDL_GetError());
	SDL_ClearError();
	StereoTexture = nullptr;
	StereoTextureSurface = nullptr;
	ReinitializeTexture();
}

void RenderStereo(SDL_Surface *monoSurface)
{
	const int width = gnScreenWidth;
	const int height = gnScreenHeight;

	if (!StereoFrameHasWorld) {
		// Menus, loading screens and videos: show the flat frame to both eyes.
		stereo::InvalidateWorld();
		if (SDL_UpdateTexture(texture.get(), nullptr, monoSurface->pixels, monoSurface->pitch) <= -1) {
			RecoverLostTextures();
			return;
		}
		// Videos swap in a texture of their own size, so size each eye from the texture.
		int eyeWidth = width;
		int eyeHeight = height;
		SDL_QueryTexture(texture.get(), nullptr, nullptr, &eyeWidth, &eyeHeight);
		const SDL_Rect leftEye { 0, 0, eyeWidth, eyeHeight };
		const SDL_Rect rightEye { eyeWidth, stereo::Settings.verticalOffset * eyeHeight / height, eyeWidth, eyeHeight };
		if (SDL_RenderCopy(renderer, texture.get(), nullptr, &leftEye) <= -1 || SDL_RenderCopy(renderer, texture.get(), nullptr, &rightEye) <= -1)
			RecoverLostTextures();
		return;
	}
	StereoFrameHasWorld = false;

	EnsureStereoResources(width, height);
	uint8_t paletteRgb[256 * 3];
	for (int i = 0; i < 256; ++i) {
		const SDL_Color &color = Palette->colors[i];
		paletteRgb[3 * i] = color.r;
		paletteRgb[3 * i + 1] = color.g;
		paletteRgb[3 * i + 2] = color.b;
	}
	const SDL_PixelFormat *format = StereoTextureSurface->format;
	const stereo::RgbFrame rgbFrame {
		static_cast<uint32_t *>(StereoTextureSurface->pixels),
		StereoTextureSurface->pitch / 4,
		format->Rshift,
		format->Gshift,
		format->Bshift,
		0xFF000000U,
		paletteRgb,
	};
	stereo::Compose(static_cast<const uint8_t *>(PalSurface->pixels), PalSurface->pitch, width, height, rgbFrame);
	if (SDL_UpdateTexture(StereoTexture.get(), nullptr, StereoTextureSurface->pixels, StereoTextureSurface->pitch) <= -1
	    || SDL_RenderCopy(renderer, StereoTexture.get(), nullptr, nullptr) <= -1) {
		RecoverLostTextures();
		return;
	}
	if (stereo::DumpNextFrame) {
		stereo::DumpNextFrame = false;
		char *basePath = SDL_GetBasePath();
		const std::string path = StrCat(basePath != nullptr ? basePath : "", "sbs_frame.bmp");
		if (SDL_SaveBMP(StereoTextureSurface.get(), path.c_str()) == 0)
			Log("Side-by-side 3D: saved {}x{} frame to {}", StereoTextureSurface->w, StereoTextureSurface->h, path);
		const std::string depthPath = StrCat(basePath != nullptr ? basePath : "", "sbs_depth.bmp");
		if (stereo::SaveDepthImage(depthPath.c_str()))
			Log("Side-by-side 3D: saved depth map to {}", depthPath);
		SDL_free(basePath);
	}
}
#endif

bool CanRenderDirectlyToOutputSurface()
{
#ifdef USE_SDL1
#ifdef SDL1_FORCE_DIRECT_RENDER
	return true;
#else
	auto *outputSurface = GetOutputSurface();
	return ((outputSurface->flags & SDL_DOUBLEBUF) == SDL_DOUBLEBUF
	    && outputSurface->w == gnScreenWidth && outputSurface->h == gnScreenHeight
	    && outputSurface->format->BitsPerPixel == 8);
#endif
#else // !USE_SDL1
	if (renderer != nullptr) return false;
	SDL_Surface *outputSurface = GetOutputSurface();
	// Assumes double-buffering is available.
	return outputSurface->w == static_cast<int>(gnScreenWidth)
	    && outputSurface->h == static_cast<int>(gnScreenHeight)
	    && SDLC_SURFACE_BITSPERPIXEL(outputSurface) == 8;
#endif
}

/**
 * @brief Limit FPS to avoid high CPU load, use when v-sync isn't available
 */
void LimitFrameRate()
{
	if (*GetOptions().Graphics.frameRateControl != FrameRateControl::CPUSleep)
		return;
	static uint32_t frameDeadline;
	const uint32_t tc = SDL_GetTicks() * 1000;
	uint32_t v = 0;
	if (frameDeadline > tc) {
		v = tc % refreshDelay;
		SDL_Delay((v / 1000) + 1); // ceil
	}
	frameDeadline = tc + v + refreshDelay;
}

} // namespace

void dx_init()
{
#ifndef USE_SDL1
	SDL_RaiseWindow(ghMainWnd);
	SDL_ShowWindow(ghMainWnd);
#endif

	Palette = SDLWrap::AllocPalette();
	palette_init();
	CreateBackBuffer();
}

Surface GlobalBackBuffer()
{
	return Surface(PalSurface, SDL_Rect { 0, 0, gnScreenWidth, gnScreenHeight });
}

void dx_cleanup()
{
#ifndef USE_SDL1
	if (ghMainWnd != nullptr)
		SDL_HideWindow(ghMainWnd);
#endif

	PalSurface = nullptr;
	PinnedPalSurface = nullptr;
	Palette = nullptr;
	RendererTextureSurface = nullptr;
#if !defined(USE_SDL1) && !defined(USE_SDL3)
	StereoTexture = nullptr;
	StereoTextureSurface = nullptr;
#endif
#ifndef USE_SDL1
	texture = nullptr;
	FreeVirtualGamepadTextures();
	if (*GetOptions().Graphics.upscale)
		SDL_DestroyRenderer(renderer);
#endif
	SDL_DestroyWindow(ghMainWnd);
}

void CreateBackBuffer()
{
	if (CanRenderDirectlyToOutputSurface()) {
		Log("{}", "Will render directly to the SDL output surface");
		PalSurface = GetOutputSurface();
		RenderDirectlyToOutputSurface = true;
	} else {
		PinnedPalSurface = SDLWrap::CreateRGBSurfaceWithFormat(
		    /*flags=*/0,
		    /*width=*/gnScreenWidth,
		    /*height=*/gnScreenHeight,
		    /*depth=*/8,
		    SDL_PIXELFORMAT_INDEX8);
		PalSurface = PinnedPalSurface.get();
	}

#if defined(USE_SDL3)
	if (!SDL_SetSurfacePalette(PalSurface, Palette.get())) ErrSdl();
#elif !defined(USE_SDL1)
	// In SDL2, `PalSurface` points to the global `palette`.
	if (SDL_SetSurfacePalette(PalSurface, Palette.get()) < 0)
		ErrSdl();
#else
	// In SDL1, `PalSurface` owns its palette and we must update it every
	// time the global `palette` is changed. No need to do anything here as
	// the global `palette` doesn't have any colors set yet.
#endif
}

void BltFast(SDL_Rect *srcRect, SDL_Rect *dstRect)
{
	if (RenderDirectlyToOutputSurface) {
#ifndef USE_SDL1
		// A null rect means the caller changed the entire surface.
		if (dstRect == nullptr || DirtyRects.size() == MaxDirtyRects)
			ForceFullUpdate = true;
		else
			DirtyRects.push_back(*dstRect);
#endif
		return;
	}
	Blit(PalSurface, srcRect, dstRect);
}

void Blit(SDL_Surface *src, SDL_Rect *srcRect, SDL_Rect *dstRect)
{
	if (HeadlessMode)
		return;

	SDL_Surface *dst = GetOutputSurface();
#if defined(USE_SDL3)
	if (!SDL_BlitSurface(src, srcRect, dst, dstRect)) ErrSdl();
#elif !defined(USE_SDL1)
	if (SDL_BlitSurface(src, srcRect, dst, dstRect) < 0)
		ErrSdl();
#else
	if (!OutputRequiresScaling()) {
		if (SDL_BlitSurface(src, srcRect, dst, dstRect) < 0)
			ErrSdl();
		return;
	}

	SDL_Rect scaledDstRect;
	if (dstRect != NULL) {
		scaledDstRect = *dstRect;
		ScaleOutputRect(&scaledDstRect);
		dstRect = &scaledDstRect;
	}

	// Same pixel format: We can call BlitScaled directly.
	if (SDLBackport_PixelFormatFormatEq(src->format, dst->format)) {
		if (SDL_BlitScaled(src, srcRect, dst, dstRect) < 0)
			ErrSdl();
		return;
	}

	// If the surface has a color key, we must stretch first and can then call BlitSurface.
	if (SDL_HasColorKey(src)) {
		SDLSurfaceUniquePtr stretched = SDLWrap::CreateRGBSurface(SDL_SWSURFACE, dstRect->w, dstRect->h, src->format->BitsPerPixel,
		    src->format->Rmask, src->format->Gmask, src->format->BitsPerPixel, src->format->Amask);
		SDL_SetColorKey(stretched.get(), SDL_SRCCOLORKEY, src->format->colorkey);
		if (src->format->palette != NULL)
			SDL_SetPalette(stretched.get(), SDL_LOGPAL, src->format->palette->colors, 0, src->format->palette->ncolors);
		SDL_Rect stretched_rect = { 0, 0, dstRect->w, dstRect->h };
		if (SDL_SoftStretch(src, srcRect, stretched.get(), &stretched_rect) < 0
		    || SDL_BlitSurface(stretched.get(), &stretched_rect, dst, dstRect) < 0) {
			ErrSdl();
		}
		return;
	}

	// A surface with a non-output pixel format but without a color key needs scaling.
	// We can convert the format and then call BlitScaled.
	SDLSurfaceUniquePtr converted = SDLWrap::ConvertSurface(src, dst->format, 0);
	if (SDL_BlitScaled(converted.get(), srcRect, dst, dstRect) < 0)
		ErrSdl();
#endif
}

#ifndef USE_SDL1
namespace {

void UpdateOutputSurface()
{

	// Only do partial blitting if the output buffer is the same as last frame.
	const SDL_Surface *outputSurface = GetOutputSurface();
	const void *pixels = outputSurface != nullptr ? outputSurface->pixels : nullptr;
	const void *lastPixels = LastPresentedPixels;
	LastPresentedPixels = pixels;

	if (!ForceFullUpdate && !DirtyRects.empty() && pixels == lastPixels) {
		const int numRects = static_cast<int>(DirtyRects.size());
#ifdef USE_SDL3
		const bool updated = SDL_UpdateWindowSurfaceRects(ghMainWnd, DirtyRects.data(), numRects);
#else
		const bool updated = SDL_UpdateWindowSurfaceRects(ghMainWnd, DirtyRects.data(), numRects) >= 0;
#endif
		DirtyRects.clear();
		if (!updated) ErrSdl();
		return;
	}
	DirtyRects.clear();
	ForceFullUpdate = false;

#ifdef USE_SDL3
	if (!SDL_UpdateWindowSurface(ghMainWnd)) ErrSdl();
#else
	if (SDL_UpdateWindowSurface(ghMainWnd) <= -1) ErrSdl();
#endif
}

} // namespace
#endif

void RenderPresent()
{
	if (HeadlessMode)
		return;

	SDL_Surface *surface = GetOutputSurface();

	if (!gbActive) {
#ifdef __EMSCRIPTEN__
		// Just yield to browser when inactive instead of blocking
		emscripten_sleep(1);
#else
		LimitFrameRate();
#endif
		return;
	}

#ifndef USE_SDL1
	if (renderer != nullptr) {
#ifdef USE_SDL3
		if (!SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255)) ErrSdl();
		if (!SDL_RenderClear(renderer)) ErrSdl();
		if (!SDL_UpdateTexture(texture.get(), nullptr, surface->pixels, surface->pitch)) ErrSdl();
		if (!SDL_RenderTexture(renderer, texture.get(), nullptr, nullptr)) ErrSdl();
#else
		if (SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255) <= -1) ErrSdl();
		if (SDL_RenderClear(renderer) <= -1) ErrSdl();
		if (stereo::Settings.enabled) {
			RenderStereo(surface);
		} else {
			if (SDL_UpdateTexture(texture.get(), nullptr, surface->pixels, surface->pitch) <= -1) ErrSdl();
			if (SDL_RenderCopy(renderer, texture.get(), nullptr, nullptr) <= -1) ErrSdl();
		}
#endif

		if (ControlMode == ControlTypes::VirtualGamepad) {
			RenderVirtualGamepad(renderer);
		}
		SDL_RenderPresent(renderer);

#ifdef __EMSCRIPTEN__
		// TODO: Refactor to use emscripten_set_main_loop or requestAnimationFrame instead.
		// For now, yield to browser to allow rendering via ASYNCIFY sleep.
		emscripten_sleep(1);
#endif

		if (*GetOptions().Graphics.frameRateControl != FrameRateControl::VerticalSync) {
			LimitFrameRate();
		}
	} else {
		if (ControlMode == ControlTypes::VirtualGamepad) {
			RenderVirtualGamepad(surface);
		}

		UpdateOutputSurface();

		if (RenderDirectlyToOutputSurface)
			PalSurface = GetOutputSurface();
		LimitFrameRate();
	}
#else
	if (SDL_Flip(surface) <= -1) {
		ErrSdl();
	}
	if (RenderDirectlyToOutputSurface)
		PalSurface = GetOutputSurface();
	LimitFrameRate();
#endif
}

} // namespace devilution
