// main_record.cpp — renders scenes offline: a saved scene's animation, or the
// scripted sequences behind the README's GIFs.
//
// Usage: tessera_record --scene <scene.json> <outdir> [--size WxH] [--fps N] [--ps1]
//    or: tessera_record --example <bounce|liquid> <outdir> [...]
//   Renders the scene's frame range through its active camera at a fixed frame
//   rate (the scene's own by default) to <outdir>/frame_0000.png, ... The same
//   scene and options always give the same frames (see Editor::renderExportFrame).
//
// Usage: tessera_record <sequence> <outdir>
//   orbit   the Stanford bunny turning, modern Phong shading
//   ps1     the textured cube turning in PS1 mode
//   liquid  the liquid demo: slide, stop, slosh, tilt
//   views   the bunny through each debug view
// Writes <outdir>/frame_0000.ppm, frame_0001.ppm, ... at 960x540.
// tools/make_gifs.sh runs this and turns the frames into GIFs with ffmpeg.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

#include "app_common.h"
#include "camera.h"
#include "editor.h"
#include "framebuffer.h"
#include "liquid_scene.h"
#include "png.h"
#include "renderer.h"

namespace {

constexpr int kWidth = 960, kHeight = 540;

bool save(const Framebuffer& fb, const std::string& dir, int frame) {
    char name[64];
    std::snprintf(name, sizeof(name), "/frame_%04d.ppm", frame);
    return fb.savePPM((dir + name).c_str());
}

int renderScene(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: tessera_record --scene <scene.json> | --example <name>  <outdir> [--size WxH] [--fps N] [--ps1]\n");
        return 1;
    }
    const std::string path = argv[2], dir = argv[3];
    int width = 1280, height = 720;
    float fps = 0;  // 0: the scene's own
    Renderer renderer;
    for (int i = 4; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--size" && i + 1 < argc && std::sscanf(argv[i + 1], "%dx%d", &width, &height) == 2) ++i;
        else if (arg == "--fps" && i + 1 < argc) fps = static_cast<float>(std::atof(argv[++i]));
        else if (arg == "--ps1") renderer.settings = RenderSettings::ps1();
        else return std::fprintf(stderr, "unknown option '%s'\n", arg.c_str()), 1;
    }
    if (width < 16 || height < 16 || width > 7680 || height > 4320) return std::fprintf(stderr, "bad --size\n"), 1;

    Editor editor;
    std::string error;
    bool loaded = false;
    if (std::string(argv[1]) == "--example") {
        loaded = editor.loadExample(path, error);
    } else {
        std::ifstream f(path);
        std::stringstream text;
        text << f.rdbuf();
        loaded = f && editor.load(text.str(), error);
        if (!f) error = "can't read the file";
    }
    if (!loaded || !editor.beginExport(error)) return std::fprintf(stderr, "%s: %s\n", path.c_str(), error.c_str()), 1;
    const AnimationSettings& a = editor.scene().animation();
    if (fps <= 0 || fps > 240) fps = a.fps;
    // The frame range in seconds, sampled at the output rate: both ends included.
    const float start = a.startFrame / a.fps, end = a.endFrame / a.fps;
    const int frames = static_cast<int>(std::lround((end - start) * fps)) + 1;

    Framebuffer fb(width, height);
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < frames; ++i) {
        editor.renderExportFrame(renderer, fb, start + i / fps, 1.0f / fps);
        char name[64];
        std::snprintf(name, sizeof(name), "/frame_%04d.png", i);
        if (!savePNG(dir + name, fb.colorData(), width, height))
            return std::fprintf(stderr, "can't write %s%s\n", dir.c_str(), name), 1;
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("%d frames at %dx%d, %.0f fps, in %.1f s (%.1f ms per frame, PNG included)\n", frames, width, height,
                fps, seconds, seconds * 1000 / frames);
    std::printf("make a video: ffmpeg -framerate %g -i %s/frame_%%04d.png -pix_fmt yuv420p out.mp4\n", fps, dir.c_str());
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1 && (std::string(argv[1]) == "--scene" || std::string(argv[1]) == "--example")) return renderScene(argc, argv);
    if (argc < 3) {
        std::fprintf(stderr, "usage: tessera_record <orbit|ps1|liquid|views> <outdir>\n"
                             "   or: tessera_record --scene <scene.json> | --example <name>  <outdir> [--size WxH] [--fps N] [--ps1]\n");
        return 1;
    }
    const std::string sequence = argv[1], dir = argv[2];
    Framebuffer fb(kWidth, kHeight);
    Renderer renderer;
    OrbitCamera camera;
    const float aspect = float(kWidth) / kHeight;

    if (sequence == "orbit" || sequence == "ps1" || sequence == "views") {
        const bool bunny = sequence != "ps1";
        Mesh mesh = bunny ? loadModelOrCube(std::string(TESSERA_ASSET_DIR) + "/bunny.obj") : makeCube();
        const Mat4 model = fitToView(mesh);
        Texture texture = defaultTexture();
        if (sequence == "ps1") renderer.settings = RenderSettings::ps1();
        camera.distance = bunny ? 2.3f : 3.4f;
        camera.pitch = 0.35f;

        // views: 5 debug views, 16 frames each, while the camera keeps turning.
        const DebugView views[] = {DebugView::None, DebugView::Wireframe, DebugView::Depth, DebugView::Overdraw,
                                   DebugView::ThreadTiles};
        const int frames = sequence == "views" ? 80 : 72;
        for (int i = 0; i < frames; ++i) {
            camera.yaw = 0.6f + 2.0f * kPi * i / frames;  // one full turn, loops seamlessly
            if (sequence == "views") {
                renderer.settings.debugView = views[i / 16];
                // Overdraw is far more telling with culling off: hidden back faces get shaded too.
                renderer.settings.backfaceCull = renderer.settings.debugView != DebugView::Overdraw;
            }
            renderer.beginFrame(fb, camera.view(), camera.projection(aspect));
            renderer.drawMesh(fb, mesh, model, {0.8f, 0.8f, 0.85f}, mesh.hasTexCoords ? &texture : nullptr);
            renderer.endFrame(fb);
            if (!save(fb, dir, i)) return 1;
        }
        return 0;
    }

    if (sequence == "liquid") {
        LiquidScene scene;
        camera.distance = 2.4f;
        camera.pitch = 0.3f;
        camera.yaw = 0.0f;
        camera.target = {0.375f, 0.0f, 0.0f};  // the middle of the tube's slide (x = 0 to 0.75)
        const float dt = 1.0f / 30.0f;  // 30 frames per second of simulated time
        for (int i = 0; i < 150; ++i) {
            const float t = i * dt;
            // Slide right, let it slosh, slide back, slosh, then tilt and level out.
            float move = 0, tilt = 0;
            if (t < 0.5f) move = 1;
            else if (t >= 1.7f && t < 2.2f) move = -1;
            else if (t >= 3.4f && t < 3.8f) tilt = 1;
            else if (t >= 4.4f && t < 4.8f) tilt = -1;
            scene.update(dt, move, tilt);
            renderer.beginFrame(fb, camera.view(), camera.projection(aspect));
            scene.draw(renderer, fb);
            renderer.endFrame(fb);
            if (!save(fb, dir, i)) return 1;
        }
        return 0;
    }

    std::fprintf(stderr, "unknown sequence '%s'\n", sequence.c_str());
    return 1;
}
