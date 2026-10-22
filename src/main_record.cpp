// main_record.cpp — renders the scripted sequences behind the README's GIFs.
//
// Usage: tessera_record <sequence> <outdir>
//   orbit   the Stanford bunny turning, modern Phong shading
//   ps1     the textured cube turning in PS1 mode
//   liquid  the liquid demo: slide, stop, slosh, tilt
//   views   the bunny through each debug view
// Writes <outdir>/frame_0000.ppm, frame_0001.ppm, ... at 960x540.
// tools/make_gifs.sh runs this and turns the frames into GIFs with ffmpeg.
#include <cstdio>
#include <string>

#include "app_common.h"
#include "camera.h"
#include "framebuffer.h"
#include "liquid_scene.h"
#include "renderer.h"

namespace {

constexpr int kWidth = 960, kHeight = 540;

bool save(const Framebuffer& fb, const std::string& dir, int frame) {
    char name[64];
    std::snprintf(name, sizeof(name), "/frame_%04d.ppm", frame);
    return fb.savePPM((dir + name).c_str());
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: tessera_record <orbit|ps1|liquid|views> <outdir>\n");
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
