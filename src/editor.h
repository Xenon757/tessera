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
#include "transform_tool.h"

class Editor {
public:
    Editor();

    // Runs one command (see commands.h); returns {"ok":true,"result":...} or
    // {"ok":false,"error":"..."}. An edit to a property that has keyframes becomes a
    // key at the current frame instead (auto-key); see executeEdit.
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
    bool loadExample(const std::string& name, std::string& error);  // "liquid", "bounce"

    // Animation playback. The playhead is UI state, like the selection: not saved
    // and not undone. Keys and the frame range are scene data, changed by commands.
    float time() const { return time_; }  // seconds
    void setTime(float seconds);          // clamped to 0 and up
    float frame() const { return time_ * scene_.animation().fps; }
    void setFrame(float frame) { setTime(frame / scene_.animation().fps); }
    bool playing() const { return playing_; }
    void setPlaying(bool on);  // starting from outside the frame range jumps to its start
    // Keys the selected object at the current frame, as one undo step: location,
    // rotation, and scale (Blender's I), or one property by its track name
    // ("position", "rotation", "scale", "color", "intensity", "fov").
    bool insertKeyframes();
    bool insertKeyframe(const std::string& property);

    // Modal grab / rotate / scale of the selected object (Blender's G, R, S), with
    // the mouse at pixel (x, y) of a width x height viewport. While it runs, each
    // update replaces the previous one inside an open undo group, so confirming
    // leaves one undo step and cancelling leaves none.
    bool beginTransform(TransformMode mode, float x, float y, int width, int height);
    bool transforming() const { return tool_.active(); }
    void transformMouse(float x, float y);
    // Keys while transforming: "x"/"y"/"z" lock an axis, digits "." "-" "Backspace"
    // type a value, "Enter" confirms, "Escape" cancels. False if the key isn't used.
    bool transformKey(const std::string& key);
    void confirmTransform();
    void cancelTransform();
    std::string transformStatus() const { return tool_.describe(); }

    // The move gizmo on the selection. Pressing on an arrow starts an axis-locked
    // grab that follows the mouse until the button is released (endGizmoDrag).
    int gizmoAxisAt(float x, float y, int width, int height) const;  // -1 if none
    void hoverGizmo(float x, float y, int width, int height);          // highlights the arrow under the mouse
    bool beginGizmoDrag(float x, float y, int width, int height);
    bool draggingGizmo() const { return gizmoDrag_; }
    void endGizmoDrag();

    // Imports an OBJ or FBX file as a new object (undoable, and selected).
    Json importModel(const std::string& path);

    // Viewport: the editor's orbit camera, or looking through the scene's active camera.
    OrbitCamera& orbit() { return orbit_; }
    void setViewThroughCamera(bool on);
    bool viewThroughCamera() const { return viewThroughCamera_; }
    void viewMatrices(float aspect, Mat4& view, Mat4& projection) const;

    // Advances playback and simulation (liquid sloshing) by dt and draws a frame.
    void render(Renderer& renderer, Framebuffer& fb, float dt);

    // Everything the panels show, as JSON. Objects carry their Euler angles too, so
    // the page never does quaternion math.
    Json state() const;
    // Changes whenever anything the UI shows changes (commands, undo, selection, load).
    uint64_t uiRevision() const { return stack_.revision() + uiChanges_; }

    const Scene& scene() const { return scene_; }

private:
    void replaceScene(Scene scene);
    void applyTransform();
    // Runs an edit through the command stack. Fields of setTransform, setMaterial,
    // setLight, and setCamera whose property has a track become addKeyframe
    // commands at the current frame (keeping that key's easing), so the edit isn't
    // overridden by the animation on the next frame; the rest run as given.
    bool executeEdit(const Json& cmd, Json& result, std::string& error);
    bool keyProperty(const Object& posed, TrackProperty property, std::string& error);
    float keyTime() const;  // the current frame's time: keys always land on whole frames

    Scene scene_;
    CommandStack stack_{scene_};
    SceneRenderer sceneRenderer_;
    OrbitCamera orbit_;
    ObjectId selected_ = kNoId;
    bool viewThroughCamera_ = false;
    uint64_t uiChanges_ = 0;
    TransformTool tool_;
    ObjectId transformTarget_ = kNoId;
    std::string transformLabel_;
    int gizmoHover_ = -1;
    bool gizmoDrag_ = false;
    float time_ = 0;
    bool playing_ = false;
};
