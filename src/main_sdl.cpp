// main_sdl.cpp — windowed front end. SDL is used ONLY to show the finished
// framebuffer and read input; all rendering happens in Renderer on the CPU.
//
// Controls:
//   Left-drag  orbit          Wheel  zoom
//   1..5       debug view (shaded, wireframe, depth, overdraw, thread tiles)
//   S          cycle shading (flat, Gouraud, Phong)
//   F          toggle texture filter (bilinear / nearest)
//   B          toggle back-face culling
//   P          toggle PS1 / modern preset (keeps the current debug view)
//   L          toggle the liquid demo (arrow keys: left/right move the tube, up/down tilt it)
//   Drop a .obj or .fbx file on the window to view it (on the web: the page's Load button)
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
#include "liquid_scene.h"
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
    // The checker shows off UV mapping on the demo cube, but on a user's model it
    // just hides the shape (their own textures aren't loaded), so uploads go plain.
    bool showTexture = true;
    bool ps1 = false;
    LiquidScene liquid;
    bool liquidMode = false;
    std::chrono::steady_clock::time_point lastFrame = std::chrono::steady_clock::now();
    bool dragging = false;
    bool running = true;

    // Frame timing, averaged and shown in the title bar twice a second.
    std::chrono::steady_clock::time_point lastTitleUpdate = std::chrono::steady_clock::now();
    double accumMs = 0.0;
    int accumFrames = 0;
};

App* gApp = nullptr;

// Swaps in a model from a file (dropped on the window, or uploaded on the web
// page). Returns a message for the user either way.
std::string loadUserModel(App& app, const std::string& path) {
    Mesh mesh;
    std::string error;
    if (!loadModel(path, mesh, error)) return "Couldn't load the model: " + error;
    if (mesh.vertices.empty()) return "Couldn't load the model: it has no triangles";
    app.mesh = std::move(mesh);
    app.model = fitToView(app.mesh);
    app.liquidMode = false;
    app.showTexture = false;
    app.camera.distance = 3.5f;
    const size_t slash = path.find_last_of('/');
    return "Loaded " + path.substr(slash == std::string::npos ? 0 : slash + 1) + " (" +
           std::to_string(app.mesh.triangleCount()) + " triangles)";
}

void handleEvent(App& app, const SDL_Event& e) {
    switch (e.type) {
        case SDL_QUIT: app.running = false; break;
        case SDL_DROPFILE:
            std::printf("%s\n", loadUserModel(app, e.drop.file).c_str());
            SDL_free(e.drop.file);
            break;
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
                case SDLK_s:
                    s.shading = s.shading == Shading::Flat      ? Shading::Gouraud
                                : s.shading == Shading::Gouraud ? Shading::Phong
                                                                : Shading::Flat;
                    break;
                case SDLK_f:
                    s.textureFilter = s.textureFilter == TextureFilter::Bilinear ? TextureFilter::Nearest
                                                                                 : TextureFilter::Bilinear;
                    break;
                case SDLK_b: s.backfaceCull = !s.backfaceCull; break;
                case SDLK_l:
                    app.liquidMode = !app.liquidMode;
                    app.camera.distance = app.liquidMode ? 3.5f : 5.0f;
                    break;
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
    const RenderSettings& s = app.renderer.settings;
    std::snprintf(title, sizeof(title), "Tessera | %s | %s | %s | %.2f ms (%.0f fps) | %d tris drawn, %d culled",
                  app.ps1 ? "PS1" : "Modern", app.liquidMode ? "Liquid" : shadingName(s.shading),
                  debugViewName(s.debugView), avg,
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

    // Real elapsed time drives the liquid simulation (capped, so a stall doesn't explode it).
    const auto now = std::chrono::steady_clock::now();
    const float dt = std::min(0.05f, std::chrono::duration<float>(now - app.lastFrame).count());
    app.lastFrame = now;
    if (app.liquidMode) {
        const Uint8* keys = SDL_GetKeyboardState(nullptr);
        const float move = static_cast<float>(keys[SDL_SCANCODE_RIGHT] - keys[SDL_SCANCODE_LEFT]);
        const float tilt = static_cast<float>(keys[SDL_SCANCODE_UP] - keys[SDL_SCANCODE_DOWN]);
        app.liquid.update(dt, move, tilt);
    }

    // Time only the CPU rendering work — that's the number you optimize.
    auto start = std::chrono::steady_clock::now();
    app.renderer.beginFrame(app.fb, app.camera.view(), app.camera.projection(float(kWidth) / kHeight));
    if (app.liquidMode)
        app.liquid.draw(app.renderer, app.fb);
    else
        app.renderer.drawMesh(app.fb, app.mesh, app.model, {0.8f, 0.8f, 0.85f},
                              app.mesh.hasTexCoords && app.showTexture ? &app.modelTexture : nullptr);
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

#ifdef __EMSCRIPTEN__
// Called from the web page (web/shell.html) after it writes an uploaded file into
// Emscripten's in-memory filesystem. Returns the message to show on the page.
extern "C" EMSCRIPTEN_KEEPALIVE const char* tessera_load_model(const char* path) {
    static std::string message;  // must outlive the call: JavaScript reads it afterwards
    message = gApp ? loadUserModel(*gApp, path) : "Still starting up, try again";
    return message.c_str();
}
#endif

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
