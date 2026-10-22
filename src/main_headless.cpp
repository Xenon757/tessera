// main_headless.cpp — renders without a window and writes a PPM image.
// Useful for benchmarking (no vsync/window overhead), regression images, and CI.
//
// Usage: tessera_headless [model.obj | liquid] [out.ppm] [frames] [threads] [distance] [simd]
//   model "liquid": the liquid demo, with a scripted motion (slide right, stop, slosh)
//   threads 0 = one per core; distance = camera distance (default 5, smaller = model fills more screen);
//   simd 1 = NEON pixel loop (default, where available), 0 = scalar loop only
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "app_common.h"
#include "camera.h"
#include "framebuffer.h"
#include "liquid_scene.h"
#include "renderer.h"

int main(int argc, char** argv) {
    std::string modelPath = argc > 1 ? argv[1] : std::string(TESSERA_ASSET_DIR) + "/cube.obj";
    std::string outPath = argc > 2 ? argv[2] : "frame.ppm";
    int frames = argc > 3 ? std::max(1, std::atoi(argv[3])) : 100;
    int threads = argc > 4 ? std::max(0, std::atoi(argv[4])) : 0;

    const int width = 960, height = 540;
    Framebuffer fb(width, height);
    Renderer renderer;
    renderer.settings.threadCount = threads;
    OrbitCamera camera;
    if (argc > 5) camera.distance = static_cast<float>(std::atof(argv[5]));
    if (argc > 6) renderer.settings.useSimd = std::atoi(argv[6]) != 0;
    const bool liquidDemo = modelPath == "liquid";
    LiquidScene liquid;
    if (liquidDemo && argc <= 5) camera.distance = 3.5f;
    Mesh mesh = liquidDemo ? Mesh{} : loadModelOrCube(modelPath);
    Mat4 model = fitToView(mesh);
    Texture texture = defaultTexture();
    const Texture* tex = mesh.hasTexCoords ? &texture : nullptr;

    using Clock = std::chrono::steady_clock;
    auto start = Clock::now();
    for (int i = 0; i < frames; ++i) {
        renderer.beginFrame(fb, camera.view(), camera.projection(float(width) / height));
        if (liquidDemo) {
            // Fixed 60 Hz steps: slide right for half a second, then let it slosh.
            const float t = (i % 180) / 60.0f;
            liquid.update(1.0f / 60.0f, t < 0.5f ? 1.0f : 0.0f, 0.0f);
            liquid.draw(renderer, fb);
        } else {
            renderer.drawMesh(fb, mesh, model, {0.8f, 0.8f, 0.85f}, tex);
        }
        renderer.endFrame(fb);
    }
    double ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count() / frames;

    const RenderStats& s = renderer.stats();
    std::printf("%d frames, avg %.3f ms/frame (%.0f fps), %d threads\n", frames, ms, 1000.0 / ms,
                renderer.threadsUsed());
    std::printf("triangles: %d submitted, %d clipped, %d culled, %d drawn\n", s.trianglesSubmitted,
                s.trianglesClipped, s.trianglesCulled, s.trianglesDrawn);

    if (!fb.savePPM(outPath.c_str())) {
        std::fprintf(stderr, "failed to write %s\n", outPath.c_str());
        return 1;
    }
    std::printf("wrote %s\n", outPath.c_str());
    return 0;
}
