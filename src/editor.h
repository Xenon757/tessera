// editor.h — the editor session behind both front ends: a scene, its command
// history, the selection, and the viewport. The web page drives it through the C
// functions in main_sdl.cpp; everything goes in and out as JSON.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "camera.h"
#include "commands.h"
#include "mesh_ops.h"
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

    // Selection is UI state, not part of the scene or its undo history: a set of
    // objects, one of them active (the one the properties panel and timeline show).
    void select(ObjectId id);        // just this one (kNoId: nothing)
    void toggleSelect(ObjectId id);  // shift-click: add it (and make it active), make it active, or drop the active one
    void selectAll(bool all);        // A / Alt+A
    ObjectId selected() const;       // the active object, or kNoId
    std::vector<ObjectId> selection() const;  // every selected object that still exists, oldest first
    // Selects what's under pixel (x, y) of a width x height viewport (or nothing);
    // with `extend` it toggles that object instead (shift-click).
    ObjectId pick(float x, float y, int width, int height, bool extend = false);
    // Box select: every visible object whose on-screen bounds overlap the rectangle
    // between two corners (pixels). With `extend` it adds to the selection.
    void boxSelect(float x0, float y0, float x1, float y1, int width, int height, bool extend);
    // Where the ray through pixel (x, y) meets the ground plane (y = 0): the spot to
    // add an object at. False if it doesn't (looking up at the sky, or too far off).
    bool groundPoint(float x, float y, int width, int height, Vec3& out) const;

    // The box being dragged, drawn as an overlay (UI state only).
    void setSelectBox(bool shown, float x0 = 0, float y0 = 0, float x1 = 0, float y1 = 0);

    // Deletes the selection as one undo step. Children that aren't deleted stay
    // where they are, moving up to their nearest surviving ancestor.
    bool deleteSelected();
    // Ctrl+P: parents the other selected objects to the active one; Alt+P: clears
    // the selection's parents. Objects stay in place; one undo step. Returns how
    // many objects changed (parenting that would make a loop is skipped).
    int parentSelection();
    int clearParents();

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
    // Loop: playback wraps from the end frame to the start (on by default); off, it
    // stops at the end. Auto-key: edits of any keyable property key it at the current
    // frame, like Blender's auto-keying (off by default; properties that already have
    // keys are keyed either way, so an edit is never silently overridden). UI state.
    bool loop() const { return loop_; }
    void setLoop(bool on);
    bool autoKey() const { return autoKey_; }
    void setAutoKey(bool on);
    // Keys the selected object at the current frame, as one undo step: location,
    // rotation, and scale (Blender's I), or one property by its track name
    // ("position", "rotation", "scale", "color", "intensity", "fov").
    bool insertKeyframes();
    bool insertKeyframe(const std::string& property);
    // Moves the playhead to the selected object's next (direction > 0) or previous
    // keyframe, or any object's when nothing is selected. False if there is none.
    bool jumpToKeyframe(int direction);

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

    // ---- Rendering out (Phase 3). Both draw with no editor overlays.
    // A still of the current frame through the active camera (or, without one, the
    // viewport's view), with the liquid as it is in the viewport.
    void renderStill(Renderer& renderer, Framebuffer& fb);
    // The animation, frame by frame, through the active camera. Each call draws the
    // scene at `time` and steps the liquid's slosh by `dt`; that simulation is
    // separate from the viewport's and restarts at beginExport. Rendering the same
    // times in the same order therefore always gives the same images, on any machine
    // (what offline rendering needs). False, with a reason, without an active camera.
    bool beginExport(std::string& error);
    void renderExportFrame(Renderer& renderer, Framebuffer& fb, float time, float dt);

    // ---- Edit mode (Phase 5): editing the active mesh object's vertices, edges, and
    // faces (Blender's Tab). The element selection is UI state, kept per stored mesh:
    // indices only mean something for the mesh they were picked on, and because an
    // edit makes a new mesh, undo returns to the old mesh and so to its old selection.
    enum class SelectMode { Vertex = 0, Edge = 1, Face = 2 };
    bool enterEditMode();  // on the active object, if it's a mesh
    void exitEditMode();
    bool editing() const { return editObject() != kNoId; }
    ObjectId editObject() const;  // kNoId when not editing (or the object went away)
    // Switching modes converts the selection: vertices to the edges and faces they
    // complete, faces or edges to their vertices (as in Blender).
    void setSelectMode(SelectMode mode);
    SelectMode selectMode() const { return selectMode_; }
    const MeshSelection& elements() const;  // the selection on the edited mesh: verts, edges, faces, all consistent
    void setElements(const MeshSelection& sel);  // in the current mode's terms (the others are derived)
    // Selects the vertex / edge nearest pixel (x, y) (within a few pixels), or the face
    // under it, skipping anything hidden behind the surface. `extend` toggles instead.
    bool pickElement(float x, float y, int width, int height, bool extend);
    void boxSelectElements(float x0, float y0, float x1, float y1, int width, int height, bool extend);
    void selectAllElements(bool all);
    // Runs a modeling operation (see mesh_ops.h) on the selection as one undo step,
    // through the editMesh command; the selection becomes what the operation leaves.
    // `params` may hold amount, cuts, and for loopcut the mouse ("at": [x, y, w, h]),
    // used when no edge is selected. Returns the command's {"ok":...} answer.
    Json meshOperation(const std::string& operation, const Json& params);
    // The "last operation" panel: re-runs the last inset / bevel / loopcut / subdivide
    // with new amount or cuts, replacing it (only if nothing has happened since).
    Json adjustLastOperation(const Json& params);
    // Extrude, then grab along the extruded faces' normal (E).
    bool extrudeAndMove(float x, float y, int width, int height);
    // A mesh's vertices and faces, for the AI assistant (get_mesh): positions rounded
    // to 3 decimals, faces as vertex index lists, and the edit selection.
    Json meshSummary(ObjectId id) const;

    // The scene in a compact form for the AI assistant (Phase 4): every object with
    // its id, name, type, parent, local transform (rotation in XYZ Euler degrees),
    // material/light/camera settings, and which properties have keys at which frames;
    // plus the playhead, frame range, active camera, and selection. Numbers are
    // rounded to 3 decimals: enough to edit by, without wasting tokens.
    Json sceneSummary() const;

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
    // Selected objects that G/R/S and the gizmo move: those without a selected
    // ancestor (moving the ancestor already carries them along).
    std::vector<ObjectId> transformRoots() const;
    Vec3 selectionPivot() const;  // the roots' average world position
    bool gizmoPivot(Vec3& pivot) const;  // where the move gizmo goes: the selection's center (edit mode: the elements')
    void setSelection(std::vector<ObjectId> ids, ObjectId active);
    float keyTime() const;  // the current frame's time: keys always land on whole frames
    // Edit mode internals (editor_edit.cpp).
    const MeshEntry* editEntry() const;
    MeshSelection completed(MeshSelection sel, SelectMode mode) const;  // derives the other element kinds
    bool beginVertexTransform(TransformMode mode, float x, float y, int width, int height);
    void applyVertexTransform();
    void confirmVertexTransform();
    bool elementCenter(Vec3& center) const;  // world center of the selected vertices
    int edgeAt(float x, float y, int width, int height) const;  // the visible edge nearest the pixel, or -1
    void showResult(const Json& result);  // selects what an editMesh result says
    Json editState() const;               // edit mode's part of state()

    Scene scene_;
    CommandStack stack_{scene_};
    SceneRenderer sceneRenderer_;
    SceneRenderer exportRenderer_;  // its own liquid simulation, for deterministic exports
    OrbitCamera orbit_;
    std::vector<ObjectId> selection_;  // in the order they were selected
    ObjectId selected_ = kNoId;        // the active one
    bool boxShown_ = false;
    float box_[4] = {0, 0, 0, 0};
    bool viewThroughCamera_ = false;
    uint64_t uiChanges_ = 0;
    TransformTool tool_;  // runs on a pivot at selectionPivot(); applyTransform applies its change to each target
    struct Target {
        ObjectId id;
        Transform start;  // world transform when the tool started
    };
    std::vector<Target> targets_;
    std::string transformLabel_;
    int gizmoHover_ = -1;
    bool gizmoDrag_ = false;
    float time_ = 0;
    bool playing_ = false;
    bool loop_ = true;
    bool autoKey_ = false;

    ObjectId editObject_ = kNoId;
    SelectMode selectMode_ = SelectMode::Vertex;
    std::map<MeshId, MeshSelection> elements_;
    // Vertex G/R/S: the vertices, their world positions when it started, and a preview
    // of the moved mesh drawn instead of the stored one until it's confirmed.
    std::vector<int> moveVerts_;
    std::vector<Vec3> moveStart_;
    std::vector<Vec3> moveLocal_;  // where they are now, in the mesh's own space
    bool vertexTransform_ = false;
    EditMesh preview_;
    Mesh previewRender_;
    struct LastOperation {
        std::string operation;
        Json params;
        uint64_t revision = 0;  // the stack's revision right after it ran
    };
    LastOperation lastOp_;
};
