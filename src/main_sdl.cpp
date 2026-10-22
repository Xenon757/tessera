// main_sdl.cpp — windowed front end. SDL is used ONLY to show the finished
// framebuffer and read input; all rendering happens in Renderer on the CPU.
//
// Controls:
//   Left-drag  orbit          Wheel  zoom
//   1..5       debug view (shaded, wireframe, depth, overdraw, thread tiles)
//   B          toggle back-face culling
//   P          toggle PS1 / modern preset (keeps the current debug view)
//   Esc        quit
//
// Usage: tessera [model.obj]
#include <SDL.h>

#include <chrono>
#include <cstdio>
#include <string>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#include "app_common.h"
#include "camera.h"
#include "framebuffer.h"
#include "renderer.h"

namespace {

constexpr int kWidth = 960;
constexpr int kHeight = 540;

struct App {
    SDL_Window* window = nullptr;
    SDL_Renderer* sdlRenderer = nullptr;
    SDL_Texture* texture = nullptr;

    Framebuffer fb{kWidth, kHeight};
    Renderer renderer;
    OrbitCamera camera;
    Mesh mesh;
    Mat4 model;
    Texture modelTexture = defaultTexture();  // used if the mesh has UVs
    bool ps1 = false;
    bool dragging = false;
    bool running = true;

    // Frame timing, averaged and shown in the title bar twice a second.
    std::chrono::steady_clock::time_point lastTitleUpdate = std::chrono::steady_clock::now();
    double accumMs = 0.0;
    int accumFrames = 0;
};

App* gApp = nullptr;

void handleEvent(App& app, const SDL_Event& e) {
    switch (e.type) {
        case SDL_QUIT: app.running = false; break;
        case SDL_MOUSEBUTTONDOWN:
            if (e.button.button == SDL_BUTTON_LEFT) app.dragging = true;
            break;
        case SDL_MOUSEBUTTONUP:
            if (e.button.button == SDL_BUTTON_LEFT) app.dragging = false;
            break;
        case SDL_MOUSEMOTION:
            if (app.dragging) app.camera.orbit(-e.motion.xrel * 0.01f, e.motion.yrel * 0.01f);
            break;
        case SDL_MOUSEWHEEL:
            app.camera.zoom(e.wheel.y > 0 ? 0.9f : 1.1f);
            break;
        case SDL_KEYDOWN: {
            RenderSettings& s = app.renderer.settings;
            switch (e.key.keysym.sym) {
                case SDLK_ESCAPE: app.running = false; break;
                case SDLK_1: s.debugView = DebugView::None; break;
                case SDLK_2: s.debugView = DebugView::Wireframe; break;
                case SDLK_3: s.debugView = DebugView::Depth; break;
                case SDLK_4: s.debugView = DebugView::Overdraw; break;
                case SDLK_5: s.debugView = DebugView::ThreadTiles; break;
                case SDLK_b: s.backfaceCull = !s.backfaceCull; break;
                case SDLK_p: {
                    DebugView keep = s.debugView;
                    app.ps1 = !app.ps1;
                    s = app.ps1 ? RenderSettings::ps1() : RenderSettings::modern();
                    s.debugView = keep;
                    break;
                }
                default: break;
            }
            break;
        }
        default: break;
    }
}

void updateTitle(App& app, double frameMs) {
    app.accumMs += frameMs;
    ++app.accumFrames;
    auto now = std::chrono::steady_clock::now();
    if (now - app.lastTitleUpdate < std::chrono::milliseconds(500)) return;

    double avg = app.accumMs / app.accumFrames;
    const RenderStats& st = app.renderer.stats();
    char title[256];
    std::snprintf(title, sizeof(title), "Tessera | %s | %s | %.2f ms (%.0f fps) | %d tris drawn, %d culled",
                  app.ps1 ? "PS1" : "Modern", debugViewName(app.renderer.settings.debugView), avg,
                  1000.0 / avg, st.trianglesDrawn, st.trianglesCulled);
    SDL_SetWindowTitle(app.window, title);
    app.lastTitleUpdate = now;
    app.accumMs = 0.0;
    app.accumFrames = 0;
}

void frame() {
    App& app = *gApp;
    SDL_Event e;
    while (SDL_PollEvent(&e)) handleEvent(app, e);

    // Time only the CPU rendering work — that's the number you optimize.
    auto start = std::chrono::steady_clock::now();
    app.renderer.beginFrame(app.fb, app.camera.view(), app.camera.projection(float(kWidth) / kHeight));
    app.renderer.drawMesh(app.fb, app.mesh, app.model, {0.8f, 0.8f, 0.85f},
                          app.mesh.hasTexCoords ? &app.modelTexture : nullptr);
    app.renderer.endFrame(app.fb);
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    updateTitle(app, ms);

    SDL_UpdateTexture(app.texture, nullptr, app.fb.colorData(), kWidth * static_cast<int>(sizeof(uint32_t)));
    SDL_RenderClear(app.sdlRenderer);
    SDL_RenderCopy(app.sdlRenderer, app.texture, nullptr, nullptr);
    SDL_RenderPresent(app.sdlRenderer);

#ifdef __EMSCRIPTEN__
    if (!app.running) emscripten_cancel_main_loop();
#endif
}

}  // namespace

int main(int argc, char** argv) {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    App app;
    gApp = &app;
    app.window = SDL_CreateWindow("Tessera", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, kWidth, kHeight,
                                  SDL_WINDOW_SHOWN | SDL_WINDOW_ALLOW_HIGHDPI);
    // No vsync on purpose: we want to see how fast the renderer actually is.
    app.sdlRenderer = app.window ? SDL_CreateRenderer(app.window, -1, 0) : nullptr;
    app.texture = app.sdlRenderer ? SDL_CreateTexture(app.sdlRenderer, SDL_PIXELFORMAT_ARGB8888,
                                                      SDL_TEXTUREACCESS_STREAMING, kWidth, kHeight)
                                  : nullptr;
    if (!app.texture) {
        std::fprintf(stderr, "SDL setup failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    std::string modelPath = argc > 1 ? argv[1] : std::string(TESSERA_ASSET_DIR) + "/cube.obj";
    app.mesh = loadModelOrCube(modelPath);
    app.model = fitToView(app.mesh);

#ifdef __EMSCRIPTEN__
    emscripten_set_main_loop(frame, 0, 1);  // browser drives the loop; never returns
#else
    while (app.running) frame();
#endif

    SDL_DestroyTexture(app.texture);
    SDL_DestroyRenderer(app.sdlRenderer);
    SDL_DestroyWindow(app.window);
    SDL_Quit();
    return 0;
}
