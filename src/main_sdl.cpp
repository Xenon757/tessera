// main_sdl.cpp — the editor's front end. SDL only shows the finished framebuffer and
// reads input; rendering happens in Renderer on the CPU, and all editing goes
// through Editor (scene + commands). On the web, the page's panels (web/editor.js)
// call the C functions at the bottom of this file.
//
// Viewport (both):
//   Left-drag  orbit          Wheel  zoom          Click  select
//   C          look through the scene camera / back to the editor view
//   1..5       debug view (shaded, wireframe, depth, overdraw, thread tiles)
//   S / F / B  shading, texture filter, back-face culling      P  PS1 preset
// Native only (on the web the page handles these, so typing in its fields works):
//   Ctrl/Cmd+Z undo, Ctrl/Cmd+Shift+Z redo, Delete or X delete, Ctrl/Cmd+S save to
//   scene.tessera.json, drop a .json scene or a .obj/.fbx model on the window, Esc quit.
//
// Usage: tessera [scene.json | model.obj | model.fbx | liquid]
#include <SDL.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#include "editor.h"
#include "framebuffer.h"

namespace {

constexpr int kWidth = 960;
constexpr int kHeight = 540;
constexpr int kClickSlop = 4;  // pixels a press may move and still count as a click

struct App {
    SDL_Window* window = nullptr;
    SDL_Renderer* sdlRenderer = nullptr;
    SDL_Texture* texture = nullptr;

    Framebuffer fb{kWidth, kHeight};
    Renderer renderer;
    Editor editor;
    bool ps1 = false;
    bool running = true;

    // A left press orbits if the mouse moves, selects if it doesn't.
    bool pressed = false, dragged = false;
    int pressX = 0, pressY = 0;
    float mouseX = kWidth * 0.5f, mouseY = kHeight * 0.5f;  // last position, in framebuffer pixels (G/R/S start here)

    std::chrono::steady_clock::time_point lastFrame = std::chrono::steady_clock::now();
    // Frame timing, averaged and shown in the title bar twice a second.
    std::chrono::steady_clock::time_point lastTitleUpdate = lastFrame;
    double accumMs = 0.0;
    int accumFrames = 0;
};

App* gApp = nullptr;

bool endsWith(const std::string& s, const char* suffix) {
    const size_t n = std::strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

// A .json file opens as a scene; anything else is imported as a model.
void openFile(App& app, const std::string& path) {
    if (endsWith(path, ".json")) {
        std::ifstream f(path);
        std::stringstream text;
        text << f.rdbuf();
        std::string error;
        if (!f || !app.editor.load(text.str(), error)) std::fprintf(stderr, "Couldn't open %s: %s\n", path.c_str(), error.c_str());
        return;
    }
    const Json r = app.editor.importModel(path);
    if (!r.find("ok")->asBool()) std::fprintf(stderr, "Couldn't import %s: %s\n", path.c_str(), r.find("error")->asString().c_str());
}

// Window coordinates scaled to framebuffer pixels (they match unless the window or
// canvas is shown at a different size).
void toFramebuffer(App& app, int wx, int wy, float& x, float& y) {
    int w = kWidth, h = kHeight;
    SDL_GetWindowSize(app.window, &w, &h);
    x = wx * float(kWidth) / w;
    y = wy * float(kHeight) / h;
}

#ifndef __EMSCRIPTEN__
// The key names Editor::transformKey understands (the web page passes the browser's).
std::string transformKeyName(SDL_Keycode k) {
    if (k == SDLK_RETURN || k == SDLK_KP_ENTER) return "Enter";
    if (k == SDLK_ESCAPE) return "Escape";
    if (k == SDLK_BACKSPACE) return "Backspace";
    if (k == SDLK_KP_0) return "0";
    if (k >= SDLK_KP_1 && k <= SDLK_KP_9) return std::string(1, static_cast<char>('1' + (k - SDLK_KP_1)));
    if (k == SDLK_KP_PERIOD) return ".";
    if (k == SDLK_KP_MINUS) return "-";
    if (k >= 32 && k < 127) return std::string(1, static_cast<char>(k));  // letters, digits, '.', '-' are ASCII
    return "";
}
#endif

void handleKey(App& app, const SDL_KeyboardEvent& key) {
    RenderSettings& s = app.renderer.settings;
    // While G/R/S runs, every key belongs to it (axis letters, numbers, Enter, Esc).
    if (app.editor.transforming()) {
#ifndef __EMSCRIPTEN__
        app.editor.transformKey(transformKeyName(key.keysym.sym));
#endif
        return;  // on the web, editor.js forwards the keys, from wherever the focus is
    }
#ifndef __EMSCRIPTEN__
    const bool command = (key.keysym.mod & (KMOD_CTRL | KMOD_GUI)) != 0;
    const bool shift = (key.keysym.mod & KMOD_SHIFT) != 0;
    if (command && key.keysym.sym == SDLK_z) {
        shift ? app.editor.redo() : app.editor.undo();
        return;
    }
    if (command && key.keysym.sym == SDLK_s) {
        std::ofstream("scene.tessera.json") << app.editor.save();
        std::printf("Saved scene.tessera.json\n");
        return;
    }
    if ((key.keysym.sym == SDLK_DELETE || key.keysym.sym == SDLK_x) && app.editor.selected() != kNoId) {
        Json cmd = Json::object();
        cmd.set("op", "removeObject");
        cmd.set("id", Json(app.editor.selected()));
        app.editor.command(cmd);
        return;
    }
    if (key.keysym.sym == SDLK_ESCAPE) app.running = false;
    if (!(key.keysym.mod & (KMOD_CTRL | KMOD_GUI | KMOD_ALT | KMOD_SHIFT))) {
        const SDL_Keycode k = key.keysym.sym;
        const TransformMode mode = k == SDLK_g   ? TransformMode::Grab
                                   : k == SDLK_r ? TransformMode::Rotate
                                   : k == SDLK_s ? TransformMode::Scale
                                                 : TransformMode::None;
        if (mode != TransformMode::None) {  // on the web, editor.js starts these
            app.editor.beginTransform(mode, app.mouseX, app.mouseY, kWidth, kHeight);
            return;
        }
    }
    // Timeline, like Blender: Space plays, Left/Right step a frame, Up/Down hop
    // between keyframes, I keys the selection. (The web page binds these itself.)
    switch (key.keysym.sym) {
        case SDLK_SPACE: app.editor.setPlaying(!app.editor.playing()); return;
        case SDLK_LEFT: app.editor.setFrame(std::round(app.editor.frame()) - 1); return;
        case SDLK_RIGHT: app.editor.setFrame(std::round(app.editor.frame()) + 1); return;
        case SDLK_UP: app.editor.jumpToKeyframe(1); return;
        case SDLK_DOWN: app.editor.jumpToKeyframe(-1); return;
        case SDLK_i: app.editor.insertKeyframes(); return;
        default: break;
    }
#endif
    if (key.keysym.mod & (KMOD_CTRL | KMOD_GUI | KMOD_ALT)) return;  // Ctrl+S saves; it shouldn't also toggle shading
    switch (key.keysym.sym) {
        case SDLK_1: s.debugView = DebugView::None; break;
        case SDLK_2: s.debugView = DebugView::Wireframe; break;
        case SDLK_3: s.debugView = DebugView::Depth; break;
        case SDLK_4: s.debugView = DebugView::Overdraw; break;
        case SDLK_5: s.debugView = DebugView::ThreadTiles; break;
        case SDLK_z:  // S is scale now; Blender keeps shading on Z too
            s.shading = s.shading == Shading::Flat ? Shading::Gouraud : s.shading == Shading::Gouraud ? Shading::Phong : Shading::Flat;
            break;
        case SDLK_f:
            s.textureFilter = s.textureFilter == TextureFilter::Bilinear ? TextureFilter::Nearest : TextureFilter::Bilinear;
            break;
        case SDLK_b: s.backfaceCull = !s.backfaceCull; break;
        case SDLK_c: app.editor.setViewThroughCamera(!app.editor.viewThroughCamera()); break;
        case SDLK_p: {
            const DebugView keep = s.debugView;
            app.ps1 = !app.ps1;
            s = app.ps1 ? RenderSettings::ps1() : RenderSettings::modern();
            s.debugView = keep;
            break;
        }
        default: break;
    }
}

void handleEvent(App& app, const SDL_Event& e) {
    switch (e.type) {
        case SDL_QUIT: app.running = false; break;
        case SDL_DROPFILE:
            openFile(app, e.drop.file);
            SDL_free(e.drop.file);
            break;
        case SDL_MOUSEBUTTONDOWN:
            if (app.editor.transforming()) {  // a click ends G/R/S: right cancels, any other keeps it
                if (e.button.button == SDL_BUTTON_RIGHT) app.editor.cancelTransform();
                else app.editor.confirmTransform();
                break;
            }
            if (e.button.button == SDL_BUTTON_LEFT) {
                float x, y;
                toFramebuffer(app, e.button.x, e.button.y, x, y);
                if (app.editor.beginGizmoDrag(x, y, kWidth, kHeight)) break;  // pressed on an arrow
                app.pressed = true;
                app.dragged = false;
                app.pressX = e.button.x;
                app.pressY = e.button.y;
            }
            break;
        case SDL_MOUSEMOTION:
            toFramebuffer(app, e.motion.x, e.motion.y, app.mouseX, app.mouseY);
            if (app.editor.transforming()) {
                app.editor.transformMouse(app.mouseX, app.mouseY);
                break;
            }
            app.editor.hoverGizmo(app.mouseX, app.mouseY, kWidth, kHeight);
            if (!app.pressed) break;
            if (std::abs(e.motion.x - app.pressX) + std::abs(e.motion.y - app.pressY) > kClickSlop) app.dragged = true;
            if (app.dragged && !app.editor.viewThroughCamera())  // the scene camera only moves by editing it
                app.editor.orbit().orbit(-e.motion.xrel * 0.01f, e.motion.yrel * 0.01f);
            break;
        case SDL_MOUSEBUTTONUP:
            if (e.button.button == SDL_BUTTON_LEFT && app.editor.draggingGizmo()) {
                app.editor.endGizmoDrag();  // releasing an arrow keeps the move
                break;
            }
            if (e.button.button != SDL_BUTTON_LEFT || !app.pressed) break;
            app.pressed = false;
            if (!app.dragged) {
                float x, y;
                toFramebuffer(app, e.button.x, e.button.y, x, y);
                app.editor.pick(x, y, kWidth, kHeight);
            }
            break;
        case SDL_MOUSEWHEEL: {
            // Trackpads send many small fractional steps. wheel.y rounds those to 0,
            // which used to count as zooming out, so zooming in jittered back and
            // forth. preciseY keeps the fraction: each whole notch zooms 10%, and
            // a fraction of a notch zooms that fraction of it (0.9^steps).
            const float steps = clampf(e.wheel.preciseY, -5.0f, 5.0f);
            if (steps != 0 && !app.editor.viewThroughCamera() && !app.editor.transforming())
                app.editor.orbit().zoom(std::pow(0.9f, steps));
            break;
        }
        case SDL_KEYDOWN: handleKey(app, e.key); break;
        default: break;
    }
}

void updateTitle(App& app, double frameMs) {
    app.accumMs += frameMs;
    ++app.accumFrames;
    const auto now = std::chrono::steady_clock::now();
    if (now - app.lastTitleUpdate < std::chrono::milliseconds(500)) return;
    const double avg = app.accumMs / app.accumFrames;
    const RenderStats& st = app.renderer.stats();
    const Object* sel = app.editor.scene().find(app.editor.selected());
    char title[256];
    std::snprintf(title, sizeof(title), "Tessera | %s | %s | frame %d%s | %s | %.2f ms (%.0f fps) | %d tris drawn",
                  app.ps1 ? "PS1" : "Modern", app.editor.viewThroughCamera() ? "Camera view" : "Editor view",
                  static_cast<int>(std::floor(app.editor.frame() + 1e-3f)), app.editor.playing() ? " (playing)" : "",
                  app.editor.transforming() ? app.editor.transformStatus().c_str() : sel ? sel->name.c_str() : "nothing selected", avg, 1000.0 / avg, st.trianglesDrawn);
    SDL_SetWindowTitle(app.window, title);
    app.lastTitleUpdate = now;
    app.accumMs = 0.0;
    app.accumFrames = 0;
}

void frame() {
    App& app = *gApp;
    SDL_Event e;
    while (SDL_PollEvent(&e)) handleEvent(app, e);

    // Real elapsed time drives simulation (capped, so a stall doesn't explode it).
    const auto now = std::chrono::steady_clock::now();
    const float dt = std::min(0.05f, std::chrono::duration<float>(now - app.lastFrame).count());
    app.lastFrame = now;

    // Time only the CPU rendering work: that's the number to optimize.
    const auto start = std::chrono::steady_clock::now();
    app.editor.render(app.renderer, app.fb, dt);
    updateTitle(app, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());

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
// ---- The web page's interface (web/editor.js). Everything is JSON in and out.
// Returned strings live in a static buffer until the next call, which is enough:
// JavaScript copies them right away.
namespace {
const char* reply(const std::string& s) {
    static std::string buffer;
    buffer = s;
    return buffer.c_str();
}
const char* state() { return reply(gApp->editor.state().dump()); }
const char* okOrError(bool ok, const std::string& error) {
    Json out = Json::object();
    out.set("ok", ok);
    if (!ok) out.set("error", error);
    return reply(out.dump());
}
}  // namespace

extern "C" {
EMSCRIPTEN_KEEPALIVE const char* tessera_command(const char* json) {
    Json cmd;
    std::string error;
    if (!Json::parse(json, cmd, error)) return okOrError(false, "bad JSON: " + error);
    return reply(gApp->editor.command(cmd).dump());
}
EMSCRIPTEN_KEEPALIVE void tessera_begin_group(const char* label) { gApp->editor.beginGroup(label); }
EMSCRIPTEN_KEEPALIVE void tessera_end_group() { gApp->editor.endGroup(); }
EMSCRIPTEN_KEEPALIVE const char* tessera_undo() { gApp->editor.undo(); return state(); }
EMSCRIPTEN_KEEPALIVE const char* tessera_redo() { gApp->editor.redo(); return state(); }
EMSCRIPTEN_KEEPALIVE const char* tessera_state() { return state(); }
EMSCRIPTEN_KEEPALIVE double tessera_ui_revision() { return static_cast<double>(gApp->editor.uiRevision()); }
EMSCRIPTEN_KEEPALIVE void tessera_select(int id) { gApp->editor.select(static_cast<ObjectId>(id < 0 ? 0 : id)); }
EMSCRIPTEN_KEEPALIVE void tessera_set_view(int throughCamera) { gApp->editor.setViewThroughCamera(throughCamera != 0); }
EMSCRIPTEN_KEEPALIVE const char* tessera_save() { return reply(gApp->editor.save()); }
EMSCRIPTEN_KEEPALIVE const char* tessera_load(const char* text) {
    std::string error;
    const bool ok = gApp->editor.load(text, error);
    return okOrError(ok, error);
}
EMSCRIPTEN_KEEPALIVE void tessera_new() { gApp->editor.newScene(); }
EMSCRIPTEN_KEEPALIVE const char* tessera_example(const char* name) {
    std::string error;
    const bool ok = gApp->editor.loadExample(name, error);
    return okOrError(ok, error);
}
// G/R/S: mode 1 grab, 2 rotate, 3 scale, starting at the last mouse position over the canvas.
EMSCRIPTEN_KEEPALIVE int tessera_transform_begin(int mode) {
    if (mode < 1 || mode > 3) return 0;
    return gApp->editor.beginTransform(static_cast<TransformMode>(mode), gApp->mouseX, gApp->mouseY, kWidth, kHeight);
}
EMSCRIPTEN_KEEPALIVE int tessera_transform_key(const char* key) { return gApp->editor.transformKey(key); }
EMSCRIPTEN_KEEPALIVE int tessera_transforming() { return gApp->editor.transforming(); }
// Timeline.
EMSCRIPTEN_KEEPALIVE void tessera_set_frame(double frame) { gApp->editor.setFrame(static_cast<float>(frame)); }
EMSCRIPTEN_KEEPALIVE void tessera_set_playing(int on) { gApp->editor.setPlaying(on != 0); }
EMSCRIPTEN_KEEPALIVE int tessera_insert_keyframes() { return gApp->editor.insertKeyframes(); }
EMSCRIPTEN_KEEPALIVE int tessera_insert_keyframe(const char* property) { return gApp->editor.insertKeyframe(property); }
EMSCRIPTEN_KEEPALIVE int tessera_jump_keyframe(int direction) { return gApp->editor.jumpToKeyframe(direction); }
EMSCRIPTEN_KEEPALIVE const char* tessera_import(const char* path) { return reply(gApp->editor.importModel(path).dump()); }
}
#endif

int main(int argc, char** argv) {
#ifdef __EMSCRIPTEN__
    // Only listen for keys on the canvas, so typing in the page's fields works.
    SDL_SetHint(SDL_HINT_EMSCRIPTEN_KEYBOARD_ELEMENT, "#canvas");
#endif
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

    if (argc > 1) {
        const std::string arg = argv[1];
        std::string error;
        if (arg == "liquid") {
            if (!app.editor.loadExample("liquid", error)) std::fprintf(stderr, "%s\n", error.c_str());
        } else {
            if (!endsWith(arg, ".json")) {  // a model: show it on its own, without the default cube
                Json cmd = Json::object();
                cmd.set("op", "removeObject");
                cmd.set("id", Json(1));
                app.editor.command(cmd);
            }
            openFile(app, arg);
        }
    }

#ifdef __EMSCRIPTEN__
    // The page's panels can call the tessera_* functions from now on.
    EM_ASM(if (Module.onEditorReady) Module.onEditorReady(););
    emscripten_set_main_loop(frame, 0, 1);  // the browser drives the loop; never returns
#else
    while (app.running) frame();
#endif
    SDL_DestroyTexture(app.texture);
    SDL_DestroyRenderer(app.sdlRenderer);
    SDL_DestroyWindow(app.window);
    SDL_Quit();
    return 0;
}
