// commands.h — the only way to change a Scene, with undo and redo.
//
// A command is a JSON object: {"op": "setTransform", "id": 3, "position": [0, 1, 0]}.
// The web UI sends these, and so will the AI: one format, one validation path.
// Running a command:
//   1. validates everything first (unknown fields included), changing nothing on error;
//   2. applies it and records its canonical form (for redo: e.g. addObject records the
//      ID it assigned, so redo recreates the same object) and its exact inverse (undo).
// Undo applies a group's inverses in reverse order; redo replays the forward forms.
//
// Ops: addObject, removeObject, setTransform, setName, setMaterial, setShader,
// setLight, setCamera, setActiveCamera, setVisible, setWorld, addKeyframe,
// removeKeyframe, moveKeyframe. See commands.cpp for each one's fields.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "json.h"
#include "scene.h"

// Validates and applies one command. On success fills `forward` (the canonical
// command, for redo), `inverse` (undoes it), `label` (for the Undo menu), and
// `result` (e.g. {"id": 4} from addObject). On failure changes nothing.
bool applyCommand(Scene& scene, const Json& command, Json& forward, Json& inverse, std::string& label, Json& result,
                  std::string& error);

class CommandStack {
public:
    explicit CommandStack(Scene& scene) : scene_(scene) {}

    const Scene& scene() const { return scene_; }

    // Runs a command and records it for undo (in the open group, if any).
    bool execute(const Json& command, Json& result, std::string& error);

    // Everything executed between beginGroup and endGroup undoes as one step: a
    // slider drag, an "add and position" action, or a whole AI reply. Groups nest;
    // the outermost label wins.
    void beginGroup(const std::string& label);
    void endGroup();

    bool undo();
    bool redo();
    bool canUndo() const { return !undo_.empty() && depth_ == 0; }
    bool canRedo() const { return !redo_.empty() && depth_ == 0; }
    std::string undoLabel() const { return undo_.empty() ? "" : undo_.back().label; }
    std::string redoLabel() const { return redo_.empty() ? "" : redo_.back().label; }

    // Forgets all history (after loading or starting a new scene).
    void reset();

    // Increments on every change to the scene or history (execute, undo, redo,
    // reset, closing a group), so a UI can tell cheaply whether it needs to refresh.
    uint64_t revision() const { return revision_; }

    static constexpr size_t kMaxUndo = 500;  // oldest steps are dropped beyond this

private:
    struct Step {
        Json forward, inverse;
    };
    struct Group {
        std::string label;
        std::vector<Step> steps;
    };
    void push(Group group);

    Scene& scene_;
    std::vector<Group> undo_, redo_;
    Group open_;  // the group being built while depth_ > 0
    int depth_ = 0;
    uint64_t revision_ = 0;
};
