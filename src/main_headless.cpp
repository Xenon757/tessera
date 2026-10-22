// main_headless.cpp — renders without a window and writes a PPM image.
// Useful for benchmarking (no vsync/window overhead), regression images, and CI.
//
// Usage: tessera_headless [model.obj] [out.ppm] [frames] [threads]   (threads 0 = one per core)
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "app_common.h"
#include "camera.h"
#include "framebuffer.h"
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
    Mesh mesh = loadModelOrCube(modelPath);
    Mat4 model = fitToView(mesh);
    Texture texture = defaultTexture();
    const Texture* tex = mesh.hasTexCoords ? &texture : nullptr;

    using Clock = std::chrono::steady_clock;
    auto start = Clock::now();
    for (int i = 0; i < frames; ++i) {
        renderer.beginFrame(fb, camera.view(), camera.projection(float(width) / height));
        renderer.drawMesh(fb, mesh, model, {0.8f, 0.8f, 0.85f}, tex);
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
