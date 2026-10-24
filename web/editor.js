// editor.js — the web editor's panels. All scene logic lives in C++ (src/editor.h);
// this file turns clicks and keystrokes into JSON commands and redraws the panels
// from tessera_state(). Plain JavaScript: no framework, no build step.
"use strict";

var TesseraEditor = (function () {
  const $ = (id) => document.getElementById(id);
  const statusEl = $("status");

  let state = null;          // the last tessera_state(), parsed
  let shownRevision = -1;    // the uiRevision that `state` reflects
  let outlinerKey = "";      // what the outliner rows were built from
  let propertiesKey = "";    // what the properties layout was built from
  let bindings = [];         // how to refresh each property input: {el, kind, get(object, state)}
  let groupOpen = false;     // an undo group is collecting edits from the focused field
  let renaming = false;      // an outliner rename box is open
  let transformShown = false; // the status line is showing a G/R/S readout

  // ------------------------------------------------------------ calls into C++

  // String arguments go through the heap: ccall would copy them onto the wasm
  // stack, which is only 64 KB, and a saved scene with meshes is far bigger.
  function call(name, ret, types, args) {
    types = types || [];
    const temps = [];
    const values = (args || []).map((a, i) => (types[i] === "string" ? (temps.push(Module.stringToNewUTF8(a)), temps.at(-1)) : a));
    try {
      return Module.ccall(name, ret, types.map((t) => (t === "string" ? "number" : t)), values);
    } finally {
      temps.forEach((p) => Module._free(p));
    }
  }
  const callJson = (name, types, args) => JSON.parse(call(name, "string", types, args));

  function showStatus(text, isError) {
    statusEl.textContent = text;
    statusEl.classList.toggle("error", !!isError);
  }

  function run(cmd) {
    const r = callJson("tessera_command", ["string"], [JSON.stringify(cmd)]);
    if (!r.ok) showStatus(r.error, true);
    else if (statusEl.classList.contains("error")) showStatus("");
    return r;
  }

  // Edits made while one field has focus (every keystroke, every color-picker move)
  // become a single undo step: a group opens with the first change and closes when
  // the field loses focus, or before anything else happens.
  function edit(label, cmd) {
    if (!groupOpen) {
      call("tessera_begin_group", null, ["string"], [label]);
      groupOpen = true;
    }
    return run(cmd);
  }
  function closeGroup() {
    if (!groupOpen) return;
    call("tessera_end_group");
    groupOpen = false;
  }

  const selectedObject = () => state && state.objects.find((o) => o.id === state.selected);

  // ------------------------------------------------------------ actions

  function undo() { closeGroup(); call("tessera_undo", "string"); }
  function redo() { closeGroup(); call("tessera_redo", "string"); }
  function select(id) { closeGroup(); call("tessera_select", null, ["number"], [id]); }

  // `at` (optional): the ground point to put it on, from a right-click. Primitives
  // are 2 units tall around their origin, so they're lifted 1 to sit on the ground.
  function addObject(kind, at) {
    closeGroup();
    const cmd = kind === "light" || kind === "camera"
      ? { op: "addObject", type: kind }
      : { op: "addObject", type: "mesh", primitive: { type: kind } };
    if (at) {
      const lift = kind === "plane" ? 0 : kind === "light" ? 3 : kind === "camera" ? 1.5 : 1;
      cmd.transform = { position: [at[0], at[1] + lift, at[2]] };
    }
    const r = run(cmd);
    if (r.ok) select(r.result.id);
  }

  // ------------------------------------------------------------ right-click menu

  // A right-click that doesn't drag (a right-drag orbits) opens an Add menu at the
  // cursor; whatever is added lands on the ground where you clicked.
  let contextAt = null;  // the ground point the open menu adds at (null: the origin)

  function openContextMenu(clientX, clientY) {
    const menu = $("context-menu");
    if (!menu.children.length) {  // the same items as the toolbar's Add menu
      menu.append(el("div", "ctx-title", "Add"));
      for (const node of $("add-menu").querySelector(".menu-items").children) menu.append(node.cloneNode(true));
    }
    const r = Module.canvas.getBoundingClientRect();
    const fx = (clientX - r.left) / r.width * 960 - 0.5, fy = (clientY - r.top) / r.height * 540 - 0.5;
    contextAt = JSON.parse(call("tessera_ground_point", "string", ["number", "number"], [fx, fy]));
    menu.hidden = false;
    // Open where clicked, but keep it inside the window.
    menu.style.left = Math.min(clientX, innerWidth - menu.offsetWidth - 8) + "px";
    menu.style.top = Math.min(clientY, innerHeight - menu.offsetHeight - 8) + "px";
    menu.querySelector("button").focus();
  }

  function closeContextMenu() { $("context-menu").hidden = true; }

  function deleteSelected() {
    if (!state.selection.length) return;
    closeGroup();
    call("tessera_delete_selected", "number");  // children of deleted objects stay, in place
  }

  function parentSelection() {
    closeGroup();
    const o = selectedObject();
    if (!o || state.selection.length < 2) return showStatus("Select the children, then shift-click the parent last, and press Ctrl P.");
    const n = call("tessera_parent_selection", "number");
    showStatus(n ? "Parented " + n + (n === 1 ? " object" : " objects") + " to " + o.name + "." : "Nothing to parent (an object can't be parented to its own child).", !n);
  }

  function clearParents() {
    closeGroup();
    const n = call("tessera_clear_parents", "number");
    if (n) showStatus("Cleared the parent of " + n + (n === 1 ? " object" : " objects") + ".");
  }

  function toggleSelect(id) { closeGroup(); call("tessera_toggle_select", null, ["number"], [id]); }

  // Replacing the scene clears the undo history, so check first.
  function okToReplace() {
    closeGroup();
    return !state.canUndo || confirm("Replace the current scene? Its undo history will be lost.");
  }

  function newScene() {
    if (!okToReplace()) return;
    call("tessera_new");
    showStatus("New scene.");
  }

  function loadExample(name) {
    if (!okToReplace()) return;
    const r = callJson("tessera_example", ["string"], [name]);
    const hints = {
      liquid: "Loaded the liquid example. The Liquid is parented to the Glass: move or rotate the Glass (G, R, or the arrows) and the liquid sloshes.",
      bounce: "Loaded the bouncing ball. Space plays and pauses; drag the keys in the timeline to retime it.",
    };
    showStatus(r.ok ? hints[name] || "Loaded the " + name + " example." : r.error, !r.ok);
  }

  function saveScene() {
    closeGroup();
    const text = call("tessera_save", "string");
    const a = document.createElement("a");
    a.href = URL.createObjectURL(new Blob([text], { type: "application/json" }));
    a.download = "scene.tessera.json";
    a.click();
    URL.revokeObjectURL(a.href);
    showStatus("Saved scene.tessera.json.");
  }

  async function openScene(file) {
    if (!file || !okToReplace()) return;
    const r = callJson("tessera_load", ["string"], [await file.text()]);
    showStatus(r.ok ? "Opened " + file.name + "." : "Couldn't open " + file.name + ": " + r.error, !r.ok);
  }

  // The browser can't hand C++ a file path, so copy the bytes into Emscripten's
  // in-memory filesystem and import from there.
  async function importModel(file) {
    if (!file) return;
    closeGroup();
    showStatus("Importing " + file.name + "…");
    const path = "/upload/" + file.name.replace(/[^\w.\-]/g, "_");
    try {
      try { Module.FS.mkdir("/upload"); } catch (e) { /* already exists */ }
      Module.FS.writeFile(path, new Uint8Array(await file.arrayBuffer()));
      const r = callJson("tessera_import", ["string"], [path]);
      showStatus(r.ok ? "Imported " + file.name + " (" + r.triangles + " triangles)." : "Couldn't import " + file.name + ": " + r.error, !r.ok);
    } finally {
      try { Module.FS.unlink(path); } catch (e) { /* never written */ }  // the mesh is in the scene now
    }
  }

  function openFile(file) {
    if (file) /\.json$/i.test(file.name) ? openScene(file) : importModel(file);
  }

  function setView(throughCamera) {
    closeGroup();
    call("tessera_set_view", null, ["number"], [throughCamera ? 1 : 0]);
  }

  // ------------------------------------------------------------ small DOM helpers

  function el(tag, className, text) {
    const e = document.createElement(tag);
    if (className) e.className = className;
    if (text !== undefined) e.textContent = text;
    return e;
  }

  const fmt = (v) => String(Math.round(v * 1000) / 1000 || 0);  // "|| 0" turns -0 into 0
  const toHex = (c) => "#" + c.map((x) => Math.round(Math.min(1, Math.max(0, x)) * 255).toString(16).padStart(2, "0")).join("");
  const fromHex = (h) => [1, 3, 5].map((i) => Math.round(parseInt(h.substr(i, 2), 16) / 255 * 1000) / 1000);

  function group(parent, title) {
    const g = el("div", "group");
    if (title) g.append(el("h3", null, title));
    parent.append(g);
    return g;
  }

  // `property` (a track name like "position") adds a keyframe button to the label.
  function field(parent, label, control, property) {
    const row = el("div", "field");
    const caption = el("label", null, label);
    if (property) caption.append(keyButton(row, property));
    row.append(caption, control);
    parent.append(row);
    return control;
  }

  // ◇ grey: not animated. ◇ yellow: animated, no key on this frame. ◆ yellow: a key
  // here. Clicking keys the property at this frame, or deletes the key that's here.
  function keyButton(row, property) {
    const b = el("button", "keybtn", "◇");
    b.type = "button";
    b.title = "Keyframe this property at the current frame";
    b.addEventListener("click", (e) => {
      e.preventDefault();
      closeGroup();
      const o = selectedObject();
      const key = o && keyAtFrame(o, property);
      if (key) run({ op: "removeKeyframe", id: o.id, property: property, time: key.time });
      else if (o) call("tessera_insert_keyframe", "number", ["string"], [property]);
    });
    bindings.push({ el: b, kind: "key", row: row, get: (o) => keyState(o, property) });
    return b;
  }

  function bind(input, kind, get) {
    bindings.push({ el: input, kind: kind, get: get });
    return input;
  }

  // A number field sends a command on every valid keystroke (so the viewport
  // follows along); Enter or Escape leaves the field, which closes the undo step.
  function numberInput(get, onValue, step, min, max) {
    const input = el("input");
    input.type = "number";
    input.step = step;
    if (min !== undefined) input.min = min;
    if (max !== undefined) input.max = max;
    input.addEventListener("input", () => {
      const v = parseFloat(input.value);
      if (isFinite(v)) onValue(v);
    });
    input.addEventListener("keydown", (e) => { if (e.key === "Enter" || e.key === "Escape") input.blur(); });
    return bind(input, "number", get);
  }

  function vec3Input(get, onVector, step) {
    const box = el("div", "vec"), inputs = [];
    const send = () => {
      const v = inputs.map((x) => parseFloat(x.value));
      if (v.every(isFinite)) onVector(v);
    };
    for (let i = 0; i < 3; ++i) inputs.push(numberInput((o, s) => get(o, s)[i], send, step));
    box.append(...inputs);
    return box;
  }

  function colorInput(get, onColor) {
    const input = el("input");
    input.type = "color";
    input.addEventListener("input", () => onColor(fromHex(input.value)));
    return bind(input, "color", get);
  }

  function textInput(get, onText) {
    const input = el("input");
    input.type = "text";
    input.maxLength = 128;
    input.addEventListener("change", () => onText(input.value));
    input.addEventListener("keydown", (e) => {
      if (e.key === "Escape") input.value = get(selectedObject(), state);  // revert, then leave
      if (e.key === "Enter" || e.key === "Escape") input.blur();
    });
    return bind(input, "text", get);
  }

  // Checkboxes and dropdowns are one discrete change each, so no group is needed.
  function checkbox(get, onChecked) {
    const input = el("input");
    input.type = "checkbox";
    input.addEventListener("change", () => { closeGroup(); onChecked(input.checked); });
    return bind(input, "checkbox", get);
  }

  function button(text, onClick) {
    const b = el("button", null, text);
    b.addEventListener("click", onClick);
    return b;
  }

  // ------------------------------------------------------------ toolbar

  function renderToolbar() {
    $("undo").disabled = !(state.canUndo || groupOpen);
    $("undo").title = state.canUndo ? "Undo " + state.undoLabel + " (Ctrl Z)" : "Undo (Ctrl Z)";
    $("redo").disabled = !state.canRedo;
    $("redo").title = state.canRedo ? "Redo " + state.redoLabel + " (Ctrl Shift Z)" : "Redo (Ctrl Shift Z)";
    $("view-editor").classList.toggle("on", !state.viewThroughCamera);
    $("view-camera").classList.toggle("on", state.viewThroughCamera);
    $("view-camera").disabled = !state.activeCamera;
  }

  // ------------------------------------------------------------ outliner

  const icons = { mesh: "▲", light: "☀", camera: "◧" };

  // Objects in tree order: each parent followed by its children, indented.
  function outlinerOrder() {
    const byParent = new Map();
    for (const o of state.objects) {
      const p = o.parent && state.objects.some((x) => x.id === o.parent) ? o.parent : 0;
      if (!byParent.has(p)) byParent.set(p, []);
      byParent.get(p).push(o);
    }
    const out = [];
    const walk = (parent, depth) => { for (const o of byParent.get(parent) || []) { out.push({ o: o, depth: depth }); walk(o.id, depth + 1); } };
    walk(0, 0);
    return out;
  }

  function renderOutliner() {
    // Rebuild the rows only when something they show changed; otherwise a click in
    // progress could land on a row that was just replaced.
    const key = JSON.stringify([state.activeCamera, state.objects.map((o) => [o.id, o.name, o.type, o.visible, o.parent || 0])]);
    if (key !== outlinerKey && !renaming) {
      outlinerKey = key;
      const list = $("outliner");
      list.textContent = "";
      if (!state.objects.length) list.append(el("div", "empty", "The scene is empty. Use Add to create an object."));
      for (const { o, depth } of outlinerOrder()) {
        const row = el("div", "row" + (o.visible ? "" : " hidden"));
        row.dataset.id = o.id;
        row.style.paddingLeft = (12 + depth * 16) + "px";
        // Drag a row onto another to parent it there (in place, like Ctrl P).
        row.draggable = true;
        row.addEventListener("dragstart", (e) => { e.dataTransfer.setData("text/x-tessera-object", String(o.id)); e.dataTransfer.effectAllowed = "move"; });
        row.addEventListener("dragover", (e) => { if (e.dataTransfer.types.includes("text/x-tessera-object")) { e.preventDefault(); row.classList.add("drop"); } });
        row.addEventListener("dragleave", () => row.classList.remove("drop"));
        row.addEventListener("drop", (e) => {
          e.preventDefault();
          e.stopPropagation();
          row.classList.remove("drop");
          const child = Number(e.dataTransfer.getData("text/x-tessera-object"));
          if (child && child !== o.id) { closeGroup(); run({ op: "setParent", id: child, parent: o.id }); }
        });
        const name = el("span", "name", o.name);
        name.title = "Double-click (or F2) to rename";
        const eye = el("button", "eye", o.visible ? "◉" : "○");
        eye.title = o.visible ? "Hide" : "Show";
        row.append(el("span", "icon", icons[o.type] || "?"), name);
        if (o.id === state.activeCamera) row.append(el("span", "tag", "active"));
        row.append(eye);
        row.addEventListener("mousedown", (e) => {
          if (e.target === eye || renaming) return;
          e.shiftKey || e.metaKey || e.ctrlKey ? toggleSelect(o.id) : select(o.id);
        });
        name.addEventListener("dblclick", () => startRename(o.id));
        eye.addEventListener("click", () => { closeGroup(); run({ op: "setVisible", id: o.id, visible: !o.visible }); });
        list.append(row);
      }
    }
    for (const row of $("outliner").children) {
      const id = Number(row.dataset.id);
      row.classList.toggle("selected", id === state.selected);
      row.classList.toggle("in-selection", id !== state.selected && state.selection.includes(id));
    }
  }

  function startRename(id) {
    const row = $("outliner").querySelector('[data-id="' + id + '"]');
    const o = state.objects.find((x) => x.id === id);
    if (!row || !o || renaming) return;
    closeGroup();
    renaming = true;
    const name = row.querySelector(".name");
    const input = el("input");
    input.type = "text";
    input.maxLength = 128;
    input.value = o.name;
    name.textContent = "";
    name.append(input);
    input.focus();
    input.select();
    let cancelled = false;
    input.addEventListener("keydown", (e) => {
      if (e.key === "Escape") cancelled = true;
      if (e.key === "Enter" || e.key === "Escape") input.blur();
    });
    input.addEventListener("blur", () => {
      renaming = false;
      outlinerKey = "";  // rebuild the row either way
      if (!cancelled && input.value && input.value !== o.name) run({ op: "setName", id: id, name: input.value });
      shownRevision = -1;
    });
  }

  // ------------------------------------------------------------ properties

  function renderProperties() {
    const o = selectedObject();
    const key = o
      ? [o.id, o.type, o.shader ? o.shader.type : "", o.meshInfo && o.meshInfo.hasUVs, o.id === state.activeCamera].join("|")
      : "world";
    if (key !== propertiesKey) {
      propertiesKey = key;
      bindings = [];
      const panel = $("properties");
      panel.textContent = "";
      if (o) buildObjectProperties(panel, o);
      else buildWorldProperties(panel);
    }
    updateValues();
  }

  // Refreshes every input from the state, except the one being typed in (and its
  // vector siblings, so a rotation being edited isn't re-expressed mid-edit).
  function updateValues() {
    const o = selectedObject();
    const active = document.activeElement;
    const frozen = active && active.parentNode && active.parentNode.classList.contains("vec") ? active.parentNode : null;
    for (const b of bindings) {
      if (b.kind === "key") {
        const k = b.get(o, state);
        b.el.textContent = k === "keyed" ? "◆" : "◇";
        b.el.className = "keybtn " + k;
        b.row.classList.toggle("animated", k === "animated");
        b.row.classList.toggle("keyed", k === "keyed");
        continue;
      }
      if (b.el === active || (frozen && b.el.parentNode === frozen)) continue;
      const v = b.get(o, state);
      if (b.kind === "number") b.el.value = fmt(v);
      else if (b.kind === "color") b.el.value = toHex(v);
      else if (b.kind === "checkbox") b.el.checked = v;
      else b.el.value = v;
    }
  }

  function buildObjectProperties(panel, o) {
    const id = o.id;
    const name = () => selectedObject().name;

    const general = group(panel);
    field(general, "Name", textInput((x) => x.name, (v) => { closeGroup(); if (v) run({ op: "setName", id: id, name: v }); }));
    field(general, "Visible", checkbox((x) => x.visible, (v) => run({ op: "setVisible", id: id, visible: v })));
    field(general, "Parent", parentSelect(id));

    const t = group(panel, "Transform");
    field(t, "Location", vec3Input((x) => x.transform.position, (v) => edit("Move " + name(), { op: "setTransform", id: id, position: v }), 0.1), "position");
    field(t, "Rotation °", vec3Input((x) => x.rotationEuler, (v) => edit("Rotate " + name(), { op: "setTransform", id: id, rotationEuler: v }), 5), "rotation");
    field(t, "Scale", vec3Input((x) => x.transform.scale, (v) => edit("Scale " + name(), { op: "setTransform", id: id, scale: v }), 0.1), "scale");

    if (o.type === "mesh") {
      const info = o.meshInfo || {};
      const m = group(panel, "Material");
      const setMaterial = (fields) => edit("Material of " + name(), Object.assign({ op: "setMaterial", id: id }, fields));
      field(m, "Color", colorInput((x) => x.material.color, (v) => setMaterial({ color: v })), "color");
      field(m, "Specular", numberInput((x) => x.material.specular, (v) => setMaterial({ specular: v }), 0.05, 0, 10));
      field(m, "Shininess", numberInput((x) => x.material.shininess, (v) => setMaterial({ shininess: v }), 4, 1, 1024));
      const checker = field(m, "Checker", checkbox((x) => x.material.checker, (v) => run({ op: "setMaterial", id: id, checker: v })));
      if (!info.hasUVs) {
        checker.disabled = true;
        m.append(el("p", "note", "This mesh has no UVs, so it can't show a texture."));
      }

      buildShaderProperties(panel, o);

      const meshNote = (info.primitive ? info.primitive[0].toUpperCase() + info.primitive.slice(1) + " primitive, " : "") +
        (info.triangles || 0).toLocaleString() + " triangles";
      group(panel, "Mesh").append(el("p", "note", meshNote));
    } else if (o.type === "light") {
      const l = group(panel, "Sun light");
      const setLight = (fields) => edit("Light " + name(), Object.assign({ op: "setLight", id: id }, fields));
      field(l, "Color", colorInput((x) => x.light.color, (v) => setLight({ color: v })), "color");
      field(l, "Intensity", numberInput((x) => x.light.intensity, (v) => setLight({ intensity: v }), 0.1, 0, 100), "intensity");
      l.append(el("p", "note", "Shines along its local −Z. The first visible light lights the scene."));
    } else if (o.type === "camera") {
      const c = group(panel, "Camera");
      const setCamera = (fields) => edit("Camera " + name(), Object.assign({ op: "setCamera", id: id }, fields));
      field(c, "Field of view", numberInput((x) => x.camera.fov, (v) => setCamera({ fov: v }), 1, 1, 170), "fov");
      field(c, "Near", numberInput((x) => x.camera.near, (v) => setCamera({ near: v }), 0.05, 0.001, 1000));
      field(c, "Far", numberInput((x) => x.camera.far, (v) => setCamera({ far: v }), 10, 0.01, 100000));
      field(c, "Look at", lookAtSelect(id));
      c.append(el("p", "note", "With a target, the camera always turns to face it, so you only animate where it stands."));
      const actions = el("div", "actions");
      if (id === state.activeCamera) {
        c.append(el("p", "note", "This is the active camera."));
      } else {
        actions.append(button("Set as active camera", () => { closeGroup(); run({ op: "setActiveCamera", id: id }); }));
      }
      actions.append(button("Look through", () => {
        closeGroup();
        if (id !== state.activeCamera && !run({ op: "setActiveCamera", id: id }).ok) return;
        setView(true);
      }));
      c.append(actions);
    }

    const actions = el("div", "actions");
    actions.append(button("Delete", deleteSelected));
    group(panel).append(actions);
  }

  // Every object that may be this one's parent: not itself, and not one of its
  // descendants (that would make a loop). Options rebuild when the scene changes.
  function parentSelect(id) {
    const select = el("select");
    const isDescendant = (candidate) => {
      for (let p = candidate; p; p = (state.objects.find((x) => x.id === p) || {}).parent || 0) if (p === id) return true;
      return false;
    };
    const fill = () => {
      const want = JSON.stringify(state.objects.map((o) => [o.id, o.name, o.parent || 0]));
      if (select.dataset.built === want) return;
      select.dataset.built = want;
      select.textContent = "";
      const none = el("option", null, "None");
      none.value = "0";
      select.append(none);
      for (const o of state.objects) {
        if (isDescendant(o.id)) continue;
        const opt = el("option", null, o.name);
        opt.value = String(o.id);
        select.append(opt);
      }
    };
    select.addEventListener("focus", fill);
    select.addEventListener("change", () => { closeGroup(); run({ op: "setParent", id: id, parent: Number(select.value) }); });
    bindings.push({ el: select, kind: "text", get: (o) => { fill(); return String(o.parent || 0); } });
    return select;
  }

  // Any other object can be a camera's target.
  function lookAtSelect(id) {
    const select = el("select");
    const fill = () => {
      const want = JSON.stringify(state.objects.map((o) => [o.id, o.name]));
      if (select.dataset.built === want) return;
      select.dataset.built = want;
      select.textContent = "";
      const none = el("option", null, "None");
      none.value = "0";
      select.append(none);
      for (const o of state.objects) {
        if (o.id === id) continue;
        const opt = el("option", null, o.name);
        opt.value = String(o.id);
        select.append(opt);
      }
    };
    select.addEventListener("change", () => { closeGroup(); run({ op: "setCamera", id: id, lookAt: Number(select.value) }); });
    bindings.push({ el: select, kind: "text", get: (o) => { fill(); return String(o.camera.lookAt || 0); } });
    return select;
  }

  function buildShaderProperties(panel, o) {
    const id = o.id;
    const name = () => selectedObject().name;
    const s = group(panel, "Shader");
    const type = el("select");
    for (const [value, label] of [["", "Standard"], ["liquid", "Liquid"], ["glass", "Glass"]]) {
      const opt = el("option", null, label);
      opt.value = value;
      type.append(opt);
    }
    field(s, "Type", bind(type, "text", (x) => (x.shader ? x.shader.type : "")));
    type.addEventListener("change", () => { closeGroup(); run({ op: "setShader", id: id, type: type.value, params: {} }); });

    // One parameter changes; the rest (like the liquid's layer colors) stay as they are.
    const setParam = (key, value) => {
      const current = selectedObject().shader;
      const params = Object.assign({}, current.params, { [key]: value });
      return edit("Shader of " + name(), { op: "setShader", id: id, type: current.type, params: params });
    };
    const param = (key, fallback) => (x) => (x.shader && key in x.shader.params ? x.shader.params[key] : fallback);
    if (o.shader && o.shader.type === "liquid") {
      field(s, "Fill level", numberInput(param("fillLevel", 0.3), (v) => setParam("fillLevel", v), 0.05, -10, 10));
      field(s, "Foam width", numberInput(param("foamWidth", 0.035), (v) => setParam("foamWidth", v), 0.005, 0, 1));
      s.append(el("p", "note", "Move or rotate the object and the liquid sloshes."));
    } else if (o.shader && o.shader.type === "glass") {
      field(s, "Tint", colorInput(param("tint", [0.8, 0.9, 1]), (v) => setParam("tint", v)));
      field(s, "Rim start", numberInput(param("rimStart", 0.6), (v) => setParam("rimStart", v), 0.05, 0, 1));
    }
  }

  function buildWorldProperties(panel) {
    panel.append(el("p", "empty", "Nothing selected. Click an object in the viewport or the outliner."));
    const w = group(panel, "World");
    field(w, "Background", colorInput((o, s) => s.world.background, (v) => edit("World settings", { op: "setWorld", background: v })));
    field(w, "Ambient", numberInput((o, s) => s.world.ambient, (v) => edit("World settings", { op: "setWorld", ambient: v }), 0.05, 0, 1));
  }

  // ------------------------------------------------------------ refresh loop

  // C++ bumps a revision counter whenever anything the panels show changes
  // (commands, undo, selection by clicking the viewport, loading). Checking it is
  // one cheap call per frame; the full state is only fetched when it moved.
  function poll() {
    const revision = call("tessera_ui_revision", "number");
    if (revision !== shownRevision) {
      state = callJson("tessera_state");
      shownRevision = revision;
      renderToolbar();
      renderOutliner();
      renderProperties();
      renderTimeline();
      renderTransformStatus();
    }
    requestAnimationFrame(poll);
  }

  // ------------------------------------------------------------ timeline

  // A small dope sheet for the selected object: a frame ruler to scrub, a Summary
  // row (every key time), and a row per animated property. Diamonds can be
  // selected, dragged to another frame, deleted (X), or given an interpolation.
  // All edits are ordinary keyframe commands, so they undo like everything else.
  const trackLabels = { position: "Location", rotation: "Rotation", scale: "Scale", color: "Color", intensity: "Intensity", fov: "Field of view" };
  let timelineKey = "";      // what the rows were built from
  let keySelection = null;   // the selected diamond: {id, property (null: summary row), time}

  const currentFrame = () => Math.round(state.frame);  // keys always land on whole frames
  const frameOf = (time) => time * state.animation.fps;
  const onFrame = (time) => Math.abs(frameOf(time) - currentFrame()) < 1e-3;
  // Frames map onto the track width minus a margin, so keys on the first and last
  // frame aren't cut in half by the edges.
  const kTrackPad = 8;
  const percent = (frame) => {
    const u = (frame - state.animation.start) / (state.animation.end - state.animation.start);
    return "calc(" + kTrackPad + "px + (100% - " + 2 * kTrackPad + "px) * " + u + ")";
  };
  const trackOf = (o, property) => (o && o.tracks ? o.tracks.find((t) => t.property === property) : null);

  function keyAtFrame(o, property) {
    const track = trackOf(o, property);
    return track ? track.keys.find((k) => onFrame(k.time)) : null;
  }

  function keyState(o, property) {
    const track = trackOf(o, property);
    if (!track) return "none";
    return track.keys.some((k) => onFrame(k.time)) ? "keyed" : "animated";
  }

  // The keys the selected diamond stands for: one, or on the Summary row, every
  // key of the object at that time.
  function selectedKeys() {
    const o = selectedObject();
    if (!keySelection || !o || o.id !== keySelection.id) return [];
    const out = [];
    for (const t of o.tracks || [])
      if (keySelection.property === null || t.property === keySelection.property)
        for (const k of t.keys) if (Math.abs(k.time - keySelection.time) < 1e-4) out.push({ property: t.property, key: k });
    return out;
  }

  // Runs a batch of commands as one undo step.
  function runGroup(label, commands) {
    closeGroup();
    call("tessera_begin_group", null, ["string"], [label]);
    for (const c of commands) run(c);
    call("tessera_end_group");
  }

  function setFrame(frame) { call("tessera_set_frame", null, ["number"], [Math.max(0, frame)]); }
  function setPlaying(on) { closeGroup(); call("tessera_set_playing", null, ["number"], [on ? 1 : 0]); }

  function insertKeyframes() {
    closeGroup();
    if (!state.selected) return showStatus("Select an object first, then press I.");
    if (call("tessera_insert_keyframes", "number"))
      showStatus("Keyed " + selectedObject().name + "'s location, rotation, and scale at frame " + currentFrame() + ".");
  }

  function deleteKeysHere() {  // Alt+I
    const o = selectedObject();
    if (!o) return;
    const commands = [];
    for (const t of o.tracks || []) for (const k of t.keys) if (onFrame(k.time)) commands.push({ op: "removeKeyframe", id: o.id, property: t.property, time: k.time });
    if (commands.length) runGroup("Delete keyframes", commands);
  }

  function deleteSelectedKeys() {
    const keys = selectedKeys();
    if (!keys.length) return false;
    runGroup("Delete keyframes", keys.map(({ property, key }) => ({ op: "removeKeyframe", id: keySelection.id, property: property, time: key.time })));
    keySelection = null;
    return true;
  }

  function moveSelectedKeys(frames) {
    const keys = selectedKeys();
    if (!keys.length || !frames) return;
    const newTime = Math.max(0, Math.round(frameOf(keySelection.time) + frames)) / state.animation.fps;
    runGroup("Move keyframes", keys.map(({ property, key }) => ({ op: "moveKeyframe", id: keySelection.id, property: property, time: key.time, newTime: newTime })));
    keySelection.time = newTime;
  }

  function setSelectedEasing(easing) {
    const keys = selectedKeys();
    runGroup("Interpolation", keys.map(({ property, key }) => ({ op: "addKeyframe", id: keySelection.id, property: property, time: key.time, value: key.value, easing: easing })));
  }

  const setIfIdle = (input, value) => { if (document.activeElement !== input) input.value = value; };

  function renderTimeline() {
    const a = state.animation, o = selectedObject();
    if (keySelection && !selectedKeys().length) keySelection = null;  // undone, deleted, or another object
    $("tl-play").textContent = state.playing ? "❚❚" : "▶";
    $("tl-loop").classList.toggle("on", state.loop);
    $("tl-autokey").classList.toggle("on", state.autoKey);
    $("tl-duration").textContent = "= " + (Math.round((a.end - a.start) / a.fps * 100) / 100) + " s";
    setIfIdle($("tl-frame"), currentFrame());
    setIfIdle($("tl-startf"), a.start);
    setIfIdle($("tl-endf"), a.end);
    setIfIdle($("tl-fps"), a.fps);
    const keys = selectedKeys();
    $("tl-easing").disabled = !keys.length;
    if (keys.length) setIfIdle($("tl-easing"), keys[0].key.easing);
    $("tl-key").disabled = !o;
    const key = JSON.stringify([o ? o.id : 0, o ? o.tracks || [] : [], a, keySelection]);
    if (key !== timelineKey) {
      timelineKey = key;
      buildTimelineRows(o);
    }
    $("tl-tracks").querySelector(".tl-playhead").style.left = percent(state.frame);
  }

  function buildTimelineRows(o) {
    const a = state.animation, labels = $("tl-labels"), tracks = $("tl-tracks");
    labels.textContent = "";
    tracks.textContent = "";
    labels.append(el("div"));  // beside the ruler
    const ruler = el("div", "tl-ruler");
    tracks.append(ruler);
    // Tick labels every 1, 2, 5, 10, 20, ... frames: the first step leaving room for them.
    const span = a.end - a.start, width = (tracks.clientWidth || 800) - 2 * kTrackPad;
    let step = 1;
    for (const s of [1, 2, 5, 10, 20, 50, 100, 200, 500, 1000, 2000, 5000, 10000]) {
      step = s;
      if (width / span * s >= 45) break;
    }
    for (let f = Math.ceil(a.start / step) * step; f <= a.end; f += step) {
      const tick = el("div", "tl-tick", String(f));
      tick.style.left = percent(f);
      ruler.append(tick);
    }
    if (!o) {
      labels.append(el("div"));
      tracks.append(el("div", "tl-empty", "Select an object to see its keyframes. Press I to key it."));
    } else {
      const all = [...new Set((o.tracks || []).flatMap((t) => t.keys.map((k) => k.time)))].sort((x, y) => x - y);
      const rows = [{ property: null, label: "Summary", times: all }].concat(
        (o.tracks || []).map((t) => ({ property: t.property, label: trackLabels[t.property] || t.property, times: t.keys.map((k) => k.time) })));
      for (const r of rows) {
        labels.append(el("div", r.property === null ? "summary" : null, r.label));
        const row = el("div", "tl-row" + (r.property === null ? " summary" : ""));
        for (const time of r.times) {
          const d = el("div", "tl-key");
          d.style.left = percent(frameOf(time));
          d.dataset.time = time;
          d.dataset.property = r.property || "";
          d.title = "Frame " + Math.round(frameOf(time)) + " — drag to move, X to delete";
          if (keySelection && keySelection.property === r.property && Math.abs(keySelection.time - time) < 1e-4) d.classList.add("selected");
          row.append(d);
        }
        tracks.append(row);
      }
    }
    tracks.append(el("div", "tl-playhead"));
  }

  function setupTimeline() {
    const tracks = $("tl-tracks");
    const frameAt = (clientX) => {
      const r = tracks.getBoundingClientRect(), a = state.animation;
      return a.start + (clientX - r.left - kTrackPad) / (r.width - 2 * kTrackPad) * (a.end - a.start);
    };
    let drag = null;
    tracks.addEventListener("pointerdown", (e) => {
      if (e.button !== 0 || !state) return;
      closeGroup();
      tracks.setPointerCapture(e.pointerId);
      const diamond = e.target.closest(".tl-key");
      if (diamond) {  // select it, and maybe drag it to another frame
        keySelection = { id: state.selected, property: diamond.dataset.property || null, time: Number(diamond.dataset.time) };
        for (const d of tracks.querySelectorAll(".tl-key.selected")) d.classList.remove("selected");
        diamond.classList.add("selected");
        timelineKey = JSON.stringify([state.selected, selectedObject().tracks || [], state.animation, keySelection]);
        drag = { diamond: diamond, startX: e.clientX, startFrame: frameOf(keySelection.time), frames: 0 };
        renderTimeline();
      } else {  // scrub
        keySelection = null;
        if (state.playing) setPlaying(false);
        drag = { scrub: true };
        setFrame(Math.round(frameAt(e.clientX)));
      }
      e.preventDefault();
    });
    tracks.addEventListener("pointermove", (e) => {
      if (!drag) return;
      if (drag.scrub) return setFrame(Math.round(frameAt(e.clientX)));
      const r = tracks.getBoundingClientRect(), a = state.animation;
      drag.frames = Math.max(Math.round((e.clientX - drag.startX) / (r.width - 2 * kTrackPad) * (a.end - a.start)), -Math.round(drag.startFrame));
      drag.diamond.style.left = percent(drag.startFrame + drag.frames);  // preview; the command runs on release
    });
    tracks.addEventListener("pointerup", () => {
      if (drag && drag.diamond && drag.frames) moveSelectedKeys(drag.frames);
      drag = null;
    });
    tracks.addEventListener("pointercancel", () => { drag = null; timelineKey = ""; });
    window.addEventListener("resize", () => { timelineKey = ""; shownRevision = -1; });  // tick spacing depends on width

    // Toolbar. Buttons give focus back, so Space doesn't also "click" them.
    const onClick = (id, fn) => $(id).addEventListener("click", (e) => { e.currentTarget.blur(); fn(); });
    onClick("tl-play", () => setPlaying(!state.playing));
    onClick("tl-start", () => { setPlaying(false); setFrame(state.animation.start); });
    onClick("tl-key", insertKeyframes);
    onClick("tl-loop", () => call("tessera_set_loop", null, ["number"], [state.loop ? 0 : 1]));
    onClick("tl-autokey", () => {
      call("tessera_set_auto_key", null, ["number"], [state.autoKey ? 0 : 1]);
      showStatus(state.autoKey ? "Auto-key off." : "Auto-key on: changing location, rotation, scale, color, or field of view keys it at the current frame.");
    });
    const onCommit = (id, fn) => {
      const input = $(id);
      input.addEventListener("change", () => { const v = Number(input.value); if (input.value !== "" && isFinite(v)) fn(v); shownRevision = -1; });
      input.addEventListener("keydown", (e) => { if (e.key === "Enter" || e.key === "Escape") input.blur(); });
    };
    onCommit("tl-frame", (v) => setFrame(Math.round(v)));
    onCommit("tl-startf", (v) => { closeGroup(); run({ op: "setAnimation", start: Math.round(v) }); });
    onCommit("tl-endf", (v) => { closeGroup(); run({ op: "setAnimation", end: Math.round(v) }); });
    onCommit("tl-fps", (v) => { closeGroup(); run({ op: "setAnimation", fps: Math.round(v) }); });
    $("tl-easing").addEventListener("change", () => setSelectedEasing($("tl-easing").value));
  }

  // ------------------------------------------------------------ G / R / S

  // The grab/rotate/scale tool runs in C++ (src/transform_tool.h) and follows the
  // mouse over the canvas through SDL. The page starts it and passes it keys,
  // because they should work wherever the focus is, not only on the canvas.
  const transformModes = { g: 1, r: 2, s: 3 };
  const transforming = () => call("tessera_transforming", "number") !== 0;

  function startTransform(key) {
    closeGroup();
    closeMenus();
    if (!call("tessera_transform_begin", "number", ["number"], [transformModes[key]]))
      showStatus("Select an object first, then press " + key.toUpperCase() + ".");
  }

  function renderTransformStatus() {
    if (state.transform) {
      showStatus(state.transform + "   ·   X / Y / Z lock an axis, type a value, click or Enter to confirm, Esc or right-click to cancel");
      transformShown = true;
    } else if (transformShown) {
      showStatus("");
      transformShown = false;
    }
  }

  // ------------------------------------------------------------ keyboard

  // True while the user is typing somewhere, so shortcuts leave the keys alone.
  function typing(target) {
    if (!target || !target.tagName) return false;
    if (target.isContentEditable || target.tagName === "TEXTAREA" || target.tagName === "SELECT") return true;
    return target.tagName === "INPUT" && !/^(checkbox|radio|button|submit|reset|color|range|file)$/.test(target.type);
  }

  function closeMenus(except) {
    for (const m of document.querySelectorAll("details.menu[open]")) if (m !== except) m.open = false;
    if (except !== $("context-menu")) closeContextMenu();
  }

  function onKeyDown(e) {
    if (document.querySelector("dialog[open]")) return;  // the Render dialog has the keyboard
    // While G/R/S runs, every key belongs to it ("x" locks the X axis, it doesn't delete).
    if (transforming()) {
      if (!e.ctrlKey && !e.metaKey) call("tessera_transform_key", "number", ["string"], [e.key]);
      e.preventDefault();
      return;
    }
    if (typing(e.target)) return;
    const mod = e.ctrlKey || e.metaKey;
    const key = e.key.toLowerCase();
    let handled = true;
    if (!mod && key === " ") {
      // A focused button would also take this Space as a click when it's released.
      if (/^(BUTTON|SUMMARY)$/.test(document.activeElement.tagName)) document.activeElement.blur();
      setPlaying(!state.playing);
    }
    else if (!mod && e.key === "ArrowLeft") setFrame(e.shiftKey ? state.animation.start : currentFrame() - 1);
    else if (!mod && e.key === "ArrowRight") setFrame(e.shiftKey ? state.animation.end : currentFrame() + 1);
    else if (!mod && (e.key === "ArrowUp" || e.key === "ArrowDown")) call("tessera_jump_keyframe", "number", ["number"], [e.key === "ArrowUp" ? 1 : -1]);
    else if (!mod && e.code === "KeyI") e.altKey ? deleteKeysHere() : insertKeyframes();  // code: Alt+I types "ˆ" on a Mac
    else if (mod && key === "z") e.shiftKey ? redo() : undo();
    else if (mod && key === "y") redo();
    else if (mod && key === "s") saveScene();
    else if (mod && key === "o") $("file-open").click();
    else if (mod && e.code === "KeyP") parentSelection();  // instead of printing the page
    else if (e.altKey && e.code === "KeyP") clearParents();  // code: Alt+P types "π" on a Mac
    else if (mod || e.altKey) handled = false;
    else if (key === "delete" || key === "backspace" || key === "x") { if (!deleteSelectedKeys()) deleteSelected(); }
    else if (key === "f2" && state.selected) startRename(state.selected);
    else if (!mod && e.code === "KeyA" && !e.shiftKey) { closeGroup(); call("tessera_select_all", null, ["number"], [e.altKey ? 0 : 1]); }
    else if (!e.shiftKey && key in transformModes) startTransform(key);
    else if (e.shiftKey && key === "a") {
      const menu = $("add-menu");
      closeMenus(menu);
      menu.open = true;
      menu.querySelector("button").focus();
    } else if (key === "escape") closeMenus();
    else handled = false;
    if (handled) e.preventDefault();
  }

  // ------------------------------------------------------------ startup

  function start() {
    document.addEventListener("keydown", onKeyDown);
    TesseraExport.init();
    TesseraChat.init();
    // A click anywhere off the canvas confirms a running G/R/S (on the canvas, C++ handles it).
    document.addEventListener("mousedown", (e) => {
      if (e.target !== Module.canvas && transforming()) call("tessera_transform_key", "number", ["string"], ["Enter"]);
    }, true);
    // Leaving a field ends its undo step; then show the values it was keeping frozen.
    document.addEventListener("focusout", () => { closeGroup(); shownRevision = -1; });

    $("undo").addEventListener("click", undo);
    $("redo").addEventListener("click", redo);
    $("view-editor").addEventListener("click", () => setView(false));
    $("view-camera").addEventListener("click", () => setView(true));

    // Menus: a click on an item runs it and closes the menu; a click elsewhere closes all.
    const actions = { new: newScene, save: saveScene, open: () => $("file-open").click(), import: () => $("file-import").click() };
    document.addEventListener("click", (e) => {
      const item = e.target.closest(".menu-items button");
      const menu = e.target.closest("details.menu");
      closeMenus(item ? null : menu);
      if (!item) return;
      if (item.dataset.action) actions[item.dataset.action]();
      if (item.dataset.add) addObject(item.dataset.add, item.closest("#context-menu") ? contextAt : null);
      if (item.dataset.example) loadExample(item.dataset.example);
      if (item.dataset.render) { closeGroup(); TesseraExport.open(item.dataset.render); }
    });

    for (const [input, handler] of [[$("file-open"), openScene], [$("file-import"), importModel]])
      input.addEventListener("change", () => { handler(input.files[0]); input.value = ""; });

    const canvas = Module.canvas;
    canvas.addEventListener("mousedown", (e) => {
      closeMenus();
      canvas.focus();
      keySelection = null;
      // SDL only sees keys typed on the canvas, so tell it about Shift (extend the
      // selection) and Alt (orbit) as they were at this press.
      call("tessera_set_mouse_modifiers", null, ["number"], [(e.shiftKey ? 1 : 0) | (e.altKey ? 2 : 0)]);
      if (e.button === 1) e.preventDefault();  // middle-drag orbits instead of auto-scrolling
    }, true);
    let rightPress = null;
    canvas.addEventListener("mousedown", (e) => {
      if (e.button === 2) rightPress = { x: e.clientX, y: e.clientY, transforming: transforming() };
    });
    document.addEventListener("mouseup", (e) => {
      if (e.button !== 2 || !rightPress) return;
      const p = rightPress;
      rightPress = null;
      // Moved: it was an orbit. During G/R/S: it cancelled the transform.
      if (!p.transforming && Math.abs(e.clientX - p.x) + Math.abs(e.clientY - p.y) <= 4) openContextMenu(e.clientX, e.clientY);
    });
    // Dropping a row on the outliner's empty space clears its parent.
    const outliner = $("outliner");
    outliner.addEventListener("dragover", (e) => { if (e.dataTransfer.types.includes("text/x-tessera-object")) e.preventDefault(); });
    outliner.addEventListener("drop", (e) => {
      const child = Number(e.dataTransfer.getData("text/x-tessera-object"));
      if (child) { e.preventDefault(); closeGroup(); run({ op: "setParent", id: child, parent: 0 }); }
    });
    setupTimeline();
    canvas.addEventListener("dragover", (e) => { e.preventDefault(); canvas.classList.add("dragging"); });
    canvas.addEventListener("dragleave", () => canvas.classList.remove("dragging"));
    canvas.addEventListener("drop", (e) => {
      e.preventDefault();
      canvas.classList.remove("dragging");
      openFile(e.dataTransfer.files[0]);
    });

    showStatus("Ready. Click an object to select it, or use Add to create one.");
    canvas.focus();
    poll();
  }

  return { start: start, call: call, closeGroup: closeGroup };
})();
