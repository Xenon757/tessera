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

  function addObject(kind) {
    closeGroup();
    const cmd = kind === "light" || kind === "camera"
      ? { op: "addObject", type: kind }
      : { op: "addObject", type: "mesh", primitive: { type: kind } };
    const r = run(cmd);
    if (r.ok) select(r.result.id);
  }

  function deleteSelected() {
    const o = selectedObject();
    if (!o) return;
    closeGroup();
    run({ op: "removeObject", id: o.id });
  }

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
    showStatus(r.ok ? "Loaded the " + name + " example. Change the Liquid's location or rotation to make it slosh." : r.error, !r.ok);
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

  function field(parent, label, control) {
    const row = el("div", "field");
    row.append(el("label", null, label), control);
    parent.append(row);
    return control;
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

  function renderOutliner() {
    // Rebuild the rows only when something they show changed; otherwise a click in
    // progress could land on a row that was just replaced.
    const key = JSON.stringify([state.activeCamera, state.objects.map((o) => [o.id, o.name, o.type, o.visible])]);
    if (key !== outlinerKey && !renaming) {
      outlinerKey = key;
      const list = $("outliner");
      list.textContent = "";
      if (!state.objects.length) list.append(el("div", "empty", "The scene is empty. Use Add to create an object."));
      for (const o of state.objects) {
        const row = el("div", "row" + (o.visible ? "" : " hidden"));
        row.dataset.id = o.id;
        const name = el("span", "name", o.name);
        name.title = "Double-click (or F2) to rename";
        const eye = el("button", "eye", o.visible ? "◉" : "○");
        eye.title = o.visible ? "Hide" : "Show";
        row.append(el("span", "icon", icons[o.type] || "?"), name);
        if (o.id === state.activeCamera) row.append(el("span", "tag", "active"));
        row.append(eye);
        row.addEventListener("mousedown", (e) => { if (e.target !== eye && !renaming) select(o.id); });
        name.addEventListener("dblclick", () => startRename(o.id));
        eye.addEventListener("click", () => { closeGroup(); run({ op: "setVisible", id: o.id, visible: !o.visible }); });
        list.append(row);
      }
    }
    for (const row of $("outliner").children) row.classList.toggle("selected", Number(row.dataset.id) === state.selected);
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

    const t = group(panel, "Transform");
    field(t, "Location", vec3Input((x) => x.transform.position, (v) => edit("Move " + name(), { op: "setTransform", id: id, position: v }), 0.1));
    field(t, "Rotation °", vec3Input((x) => x.rotationEuler, (v) => edit("Rotate " + name(), { op: "setTransform", id: id, rotationEuler: v }), 5));
    field(t, "Scale", vec3Input((x) => x.transform.scale, (v) => edit("Scale " + name(), { op: "setTransform", id: id, scale: v }), 0.1));

    if (o.type === "mesh") {
      const info = o.meshInfo || {};
      const m = group(panel, "Material");
      const setMaterial = (fields) => edit("Material of " + name(), Object.assign({ op: "setMaterial", id: id }, fields));
      field(m, "Color", colorInput((x) => x.material.color, (v) => setMaterial({ color: v })));
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
      field(l, "Color", colorInput((x) => x.light.color, (v) => setLight({ color: v })));
      field(l, "Intensity", numberInput((x) => x.light.intensity, (v) => setLight({ intensity: v }), 0.1, 0, 100));
      l.append(el("p", "note", "Shines along its local −Z. The first visible light lights the scene."));
    } else if (o.type === "camera") {
      const c = group(panel, "Camera");
      const setCamera = (fields) => edit("Camera " + name(), Object.assign({ op: "setCamera", id: id }, fields));
      field(c, "Field of view", numberInput((x) => x.camera.fov, (v) => setCamera({ fov: v }), 1, 1, 170));
      field(c, "Near", numberInput((x) => x.camera.near, (v) => setCamera({ near: v }), 0.05, 0.001, 1000));
      field(c, "Far", numberInput((x) => x.camera.far, (v) => setCamera({ far: v }), 10, 0.01, 100000));
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
      renderTransformStatus();
    }
    requestAnimationFrame(poll);
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
  }

  function onKeyDown(e) {
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
    if (mod && key === "z") e.shiftKey ? redo() : undo();
    else if (mod && key === "y") redo();
    else if (mod && key === "s") saveScene();
    else if (mod && key === "o") $("file-open").click();
    else if (mod || e.altKey) handled = false;
    else if (key === "delete" || key === "backspace" || key === "x") deleteSelected();
    else if (key === "f2" && state.selected) startRename(state.selected);
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
      if (item.dataset.add) addObject(item.dataset.add);
      if (item.dataset.example) loadExample(item.dataset.example);
    });

    for (const [input, handler] of [[$("file-open"), openScene], [$("file-import"), importModel]])
      input.addEventListener("change", () => { handler(input.files[0]); input.value = ""; });

    const canvas = Module.canvas;
    canvas.addEventListener("mousedown", () => { closeMenus(); canvas.focus(); });
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

  return { start: start };
})();
