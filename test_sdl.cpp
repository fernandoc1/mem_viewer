#include "mem_viewer.h"

#include <SDL2/SDL.h>

#include <cstdint>
#include <cstdio>
#include <vector>

int main() {
    std::vector<uint8_t> buffer(256, 0);
    MemViewer *viewer = mem_viewer_open(buffer.data(), buffer.size());
    if (!viewer) {
        std::fprintf(stderr, "mem_viewer_open failed\n");
        return 1;
    }

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        mem_viewer_destroy(viewer);
        return 1;
    }

    SDL_Window *window = SDL_CreateWindow("SDL Shared Buffer Demo", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 800, 480, 0);
    SDL_Renderer *renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!window || !renderer) {
        std::fprintf(stderr, "SDL setup failed: %s\n", SDL_GetError());
        mem_viewer_destroy(viewer);
        SDL_Quit();
        return 1;
    }

    uint32_t tick = 0;
    bool running = true;
    while (running && mem_viewer_is_open(viewer)) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                running = false;
            }
        }

        buffer[0] += 1;
        buffer[1] += 2;
        buffer[2] ^= 3;
        buffer[3] += 1;
        buffer[4] = (uint8_t)((tick / 2) & 0xff);
        buffer[5] = (uint8_t)((tick * 3) & 0xff);
        buffer[6] = (uint8_t)((tick * 5) & 0xff);
        buffer[7] = (uint8_t)((tick * 7) & 0xff);
        ++tick;

        const int x = buffer[0] * 2;
        const int y = buffer[1];
        const int w = 40 + (buffer[2] % 180);
        const int h = 40 + (buffer[3] % 140);

        SDL_SetRenderDrawColor(renderer, 18, 18, 22, 255);
        SDL_RenderClear(renderer);

        SDL_Rect rect = {x % 760, y % 440, w, h};
        SDL_SetRenderDrawColor(renderer, buffer[4], buffer[5], buffer[6], 255);
        SDL_RenderFillRect(renderer, &rect);

        SDL_SetRenderDrawColor(renderer, 255 - buffer[6], 180, buffer[7], 255);
        for (int i = 0; i < 8; ++i) {
            SDL_RenderDrawLine(renderer, 0, i * 60, 799, (i * 60 + buffer[i]) % 480);
        }

        SDL_RenderPresent(renderer);
        SDL_Delay(16);
    }

    mem_viewer_destroy(viewer);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
