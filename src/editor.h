// editor.h — the editor session behind both front ends: a scene, its command
// history, the selection, and the viewport. The web page drives it through the C
// functions in main_sdl.cpp; everything goes in and out as JSON.
#pragma once

#include <cstdint>
#include <string>

#include "camera.h"
#include "commands.h"
#include "renderer.h"
#include "scene.h"
#include "scene_render.h"

class Editor {
public:
    Editor();

    // Runs one command (see commands.h); returns {"ok":true,"result":...} or
    // {"ok":false,"error":"..."}.
    Json command(const Json& cmd);
    void beginGroup(const std::string& label) { stack_.beginGroup(label); }
    void endGroup() { stack_.endGroup(); }
    bool undo();
    bool redo();

    // Selection is UI state, not part of the scene or its undo history.
    void select(ObjectId id);
    ObjectId selected() const { return selected_; }
    // Selects whatever is under pixel (x, y) of a width x height viewport (or nothing).
    ObjectId pick(float x, float y, int width, int height);

    // Scenes. load/newScene/loadExample replace the scene and clear the history.
    std::string save() const;
    bool load(const std::string& text, std::string& error);
    void newScene();
    bool loadExample(const std::string& name, std::string& error);  // "liquid"

    // Imports an OBJ or FBX file as a new object (undoable, and selected).
    Json importModel(const std::string& path);

    // Viewport: the editor's orbit camera, or looking through the scene's active camera.
    OrbitCamera& orbit() { return orbit_; }
    void setViewThroughCamera(bool on);
    bool viewThroughCamera() const { return viewThroughCamera_; }
    void viewMatrices(float aspect, Mat4& view, Mat4& projection) const;

    // Advances simulation (liquid sloshing) and draws a frame.
    void render(Renderer& renderer, Framebuffer& fb, float dt);

    // Everything the panels show, as JSON. Objects carry their Euler angles too, so
    // the page never does quaternion math.
    Json state() const;
    // Changes whenever anything the UI shows changes (commands, undo, selection, load).
    uint64_t uiRevision() const { return stack_.revision() + uiChanges_; }

    const Scene& scene() const { return scene_; }

private:
    void replaceScene(Scene scene);

    Scene scene_;
    CommandStack stack_{scene_};
    SceneRenderer sceneRenderer_;
    OrbitCamera orbit_;
    ObjectId selected_ = kNoId;
    bool viewThroughCamera_ = false;
    uint64_t uiChanges_ = 0;
};
