#pragma once

#ifdef USE_SDL3
#include <SDL3/SDL_events.h>
#else
#include <SDL.h>
#endif

#include "controls/controller.h"
#include "controls/controller_motion.h"
#include "engine/render/stereo.hpp"
#include "utils/display.h"

namespace devilution {

/** Both halves of a side-by-side stereo image map onto the same game coordinates. */
inline void FoldStereoMouseEvent(SDL_Event &event)
{
#if !defined(USE_SDL1) && !defined(USE_SDL3)
	if (!stereo::Settings.enabled)
		return;
	if (event.type == SDL_MOUSEMOTION && event.motion.x >= gnScreenWidth) {
		event.motion.x -= gnScreenWidth;
	} else if ((event.type == SDL_MOUSEBUTTONDOWN || event.type == SDL_MOUSEBUTTONUP) && event.button.x >= gnScreenWidth) {
		event.button.x -= gnScreenWidth;
	}
#endif
}

inline bool PollEvent(SDL_Event *event)
{
#ifdef USE_SDL3
	const bool result = SDL_PollEvent(event);
#else
	const bool result = SDL_PollEvent(event) != 0;
#endif
	if (result) {
		FoldStereoMouseEvent(*event);
		UnlockControllerState(*event);
		ProcessControllerMotion(*event);
	}

	return result;
}

} // namespace devilution
