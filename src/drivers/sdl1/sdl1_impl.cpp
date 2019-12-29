#include "sdl1_impl.h"

#include "sdl1_audio.h"
#include "throttle.h"

#include <core.h>

#include <SDL.h>

#include <cstdint>
#include <unistd.h>

namespace drivers {

bool sdl1_impl::init() {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_JOYSTICK | SDL_INIT_NOPARACHUTE) != 0) {
        return false;
    }
    SDL_WM_SetCaption("SDLRetro", nullptr);
    audio = std::make_unique<sdl1_audio>();
    return SDL_SetVideoMode(640, 480, 16, SDL_SWSURFACE | SDL_DOUBLEBUF) != nullptr;
}

void sdl1_impl::deinit() {
    SDL_Quit();
}

bool sdl1_impl::run_frame() {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
            case SDL_QUIT:
                return false;
            default: break;
        }
    }
    return true;
}

void sdl1_impl::geometry_updated() {
    if (base_width != 0 && base_height != 0)
        SDL_SetVideoMode(base_width, base_height, 16, SDL_SWSURFACE | SDL_DOUBLEBUF);
    else
        SDL_SetVideoMode(640, 480, 16, SDL_SWSURFACE | SDL_DOUBLEBUF);
}

void sdl1_impl::video_refresh(const void *data, unsigned width, unsigned height, size_t pitch) {
    uint64_t usecs = 0;
    do {
        usleep(usecs);
        usecs = frame_throttle->check_wait();
    } while(usecs);

    if (!data) return;

    auto *surface = SDL_GetVideoSurface();
    bool lock = SDL_MUSTLOCK(surface);
    if (lock) SDL_LockSurface(surface);
    auto *pixels = static_cast<uint8_t*>(surface->pixels);
    const auto *input = static_cast<const uint8_t*>(data);
    int output_pitch = surface->pitch;
    int h = static_cast<int>(height);
    for (; h; h--) {
        memcpy(pixels, input, width * 2);
        pixels += output_pitch;
        input += pitch;
    }
    if (lock) SDL_UnlockSurface(surface);
    SDL_Flip(surface);
}

void sdl1_impl::input_poll() {
    int numkeys;
    uint8_t *keys = SDL_GetKeyState(&numkeys);
    uint16_t state = 0;
    if (keys[SDLK_w]) state |= 1U << RETRO_DEVICE_ID_JOYPAD_UP;
    if (keys[SDLK_a]) state |= 1U << RETRO_DEVICE_ID_JOYPAD_LEFT;
    if (keys[SDLK_s]) state |= 1U << RETRO_DEVICE_ID_JOYPAD_DOWN;
    if (keys[SDLK_d]) state |= 1U << RETRO_DEVICE_ID_JOYPAD_RIGHT;
    if (keys[SDLK_i]) state |= 1U << RETRO_DEVICE_ID_JOYPAD_X;
    if (keys[SDLK_j]) state |= 1U << RETRO_DEVICE_ID_JOYPAD_Y;
    if (keys[SDLK_k]) state |= 1U << RETRO_DEVICE_ID_JOYPAD_B;
    if (keys[SDLK_l]) state |= 1U << RETRO_DEVICE_ID_JOYPAD_A;
    if (keys[SDLK_c]) state |= 1U << RETRO_DEVICE_ID_JOYPAD_SELECT;
    if (keys[SDLK_v]) state |= 1U << RETRO_DEVICE_ID_JOYPAD_START;
    if (keys[SDLK_q]) state |= 1U << RETRO_DEVICE_ID_JOYPAD_L;
    if (keys[SDLK_e]) state |= 1U << RETRO_DEVICE_ID_JOYPAD_R;
    if (keys[SDLK_1]) state |= 1U << RETRO_DEVICE_ID_JOYPAD_L2;
    if (keys[SDLK_3]) state |= 1U << RETRO_DEVICE_ID_JOYPAD_R2;
    if (keys[SDLK_z]) state |= 1U << RETRO_DEVICE_ID_JOYPAD_L3;
    if (keys[SDLK_x]) state |= 1U << RETRO_DEVICE_ID_JOYPAD_R3;
    pad_states = static_cast<int16_t>(state);
}

}