// chat.js — the AI chat panel. You describe a change; Claude makes it with the
// editor's own scene commands. The agent loop runs here in the page: each turn goes
// to the Tessera Worker (worker/), which holds the API key and calls Claude, and the
// tool calls that come back run through the same validated command path as the UI
// (tessera_command), results going back as the next turn. One whole reply is one
// undo step.
"use strict";

var TesseraChat = (function () {
  const $ = (id) => document.getElementById(id);
  const MAX_ROUNDS = 16;  // model turns per message: plenty for a scene edit, and a bound on a runaway loop
  const COMMANDS = new Set(["addObject", "removeObject", "setTransform", "setName", "setMaterial", "setShader",
    "setLight", "setCamera", "setActiveCamera", "setVisible", "setParent", "setWorld", "setAnimation",
    "addKeyframe", "removeKeyframe", "moveKeyframe", "editMesh"]);
  const VERBS = { addObject: "Add", removeObject: "Delete", setTransform: "Transform", setName: "Rename",
    setMaterial: "Material of", setShader: "Shader of", setLight: "Light", setCamera: "Camera",
    setActiveCamera: "Active camera:", setVisible: "Visibility of", setParent: "Parent", setWorld: "World settings",
    setAnimation: "Animation settings", addKeyframe: "Keyframe", removeKeyframe: "Delete keyframe",
    moveKeyframe: "Move keyframe", editMesh: "Edit mesh of", get_scene: "Read the scene", get_mesh: "Read the mesh of" };
  const OPERATIONS = { extrude: "extrude", inset: "inset", bevel: "bevel", loopcut: "loop cut", subdivide: "subdivide",
    merge: "merge", delete: "delete", move: "move vertices" };

  let messages = [];  // the conversation, exactly as the API returned it (append-only)
  let job = null;     // {controller} while a reply is in progress

  const call = (...args) => TesseraEditor.call(...args);
  const sceneJson = () => call("tessera_scene_summary", "string");

  // The Worker: the deployed one (set at deploy time, see pages.yml), one entered in
  // Settings, or a local `wrangler dev` when the page itself is served locally.
  function workerUrl() {
    let saved = "";
    try { saved = localStorage.getItem("tessera-chat-url") || ""; } catch (e) { /* storage blocked */ }
    if (saved) return saved;
    if (typeof tesseraChatUrl === "string" && tesseraChatUrl && !/^__/.test(tesseraChatUrl)) return tesseraChatUrl;
    if (/^(localhost|127\.0\.0\.1)$/.test(location.hostname)) return "http://127.0.0.1:8787/chat";
    return "";
  }

  // ------------------------------------------------------------ the log

  function add(className, text) {
    const div = document.createElement("div");
    div.className = "chat-" + className;
    div.textContent = text;
    $("chat-log").append(div);
    $("chat-log").scrollTop = $("chat-log").scrollHeight;
    return div;
  }

  function setStatus(text) { $("chat-status").textContent = text; }

  // A readable line for one tool call: the verb, and which object it touched.
  function describe(use, names, result) {
    const input = use.input || {};
    const target = result && result.name ? result.name : names.get(input.id) || (input.id !== undefined ? "#" + input.id : "");
    let line = (VERBS[use.name] || use.name) + (target ? " " + target : "");
    if (use.name === "addKeyframe" || use.name === "removeKeyframe" || use.name === "moveKeyframe") {
      const fps = JSON.parse(call("tessera_state", "string")).animation.fps;
      line += " " + input.property + " at frame " + Math.round((input.time || 0) * fps) + (input.newTime !== undefined ? " → " + Math.round(input.newTime * fps) : "");
    }
    if (use.name === "editMesh") line += " (" + (OPERATIONS[input.operation] || input.operation) + ")";
    if (use.name === "setParent") line += input.parent ? " to " + (names.get(input.parent) || "#" + input.parent) : " (cleared)";
    return line;
  }

  // ------------------------------------------------------------ running tools

  // Every call goes through tessera_command: validated like any UI edit (unknown
  // fields, ranges, ids, loops), applied, and recorded for undo. Failures go back to
  // Claude as errors so it can fix the call.
  function runTool(use, names) {
    let ok = false, content = "", result = null;
    if (use.name === "get_scene") {
      ok = true;
      content = sceneJson();
    } else if (use.name === "get_mesh") {
      const id = Number((use.input || {}).id);
      const r = JSON.parse(call("tessera_mesh_summary", "string", ["number"], [Number.isInteger(id) && id > 0 ? id : 0]));
      ok = r.ok;
      content = ok ? JSON.stringify(r) : r.error;
    } else if (!COMMANDS.has(use.name)) {
      content = "unknown tool " + use.name;
    } else {
      const command = Object.assign({}, use.input, { op: use.name });  // the tool name decides the command
      const r = JSON.parse(call("tessera_command", "string", ["string"], [JSON.stringify(command)]));
      ok = r.ok;
      result = r.result || null;
      content = ok ? JSON.stringify(result || {}) : r.error;
    }
    add(ok ? "action" : "action error", (ok ? "✓ " : "✗ ") + describe(use, names, result) + (ok ? "" : ": " + content));
    return { type: "tool_result", tool_use_id: use.id, content: content, ...(ok ? {} : { is_error: true }) };
  }

  // ------------------------------------------------------------ one message

  async function send(text) {
    const url = workerUrl();
    if (!url) {
      $("chat-settings").open = true;
      return add("note error", "The chat isn't connected yet: enter your Worker's URL in Settings below.");
    }
    add("user", text);
    // Each message carries the scene as it is now, so edits made by hand in between count.
    messages.push({ role: "user", content: [{ type: "text", text: "Current scene:\n" + sceneJson() }, { type: "text", text: text }] });

    TesseraEditor.closeGroup();
    call("tessera_begin_group", null, ["string"], ["AI: " + (text.length > 40 ? text.slice(0, 37) + "…" : text)]);
    job = { controller: new AbortController() };
    $("chat-send").disabled = true;
    $("chat-stop").hidden = false;
    const started = performance.now();
    const ticker = setInterval(() => setStatus("Claude is working… " + Math.round((performance.now() - started) / 1000) + " s"), 500);
    try {
      for (let round = 0; round < MAX_ROUNDS; ++round) {
        setStatus("Claude is working…");
        const response = await fetch(url, {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({ messages: messages }),
          signal: job.controller.signal,
        });
        const data = await response.json().catch(() => ({ error: "the Worker sent something that isn't JSON" }));
        if (!response.ok) throw new Error(data.error || "the Worker answered " + response.status);

        // Kept exactly as returned (thinking blocks included): the next request must
        // replay the conversation unchanged.
        messages.push({ role: "assistant", content: data.content });
        for (const block of data.content) if (block.type === "text" && block.text.trim()) add("assistant", block.text.trim());
        if (data.stop_reason === "refusal") { add("note error", "Claude declined this request."); break; }

        const uses = data.content.filter((b) => b.type === "tool_use");
        if (!uses.length) {
          if (data.stop_reason === "max_tokens") add("note", "(The reply was cut off at its length limit.)");
          break;
        }
        const names = new Map(JSON.parse(call("tessera_state", "string")).objects.map((o) => [o.id, o.name]));
        messages.push({ role: "user", content: uses.map((use) => runTool(use, names)) });  // all results in one message
        if (round === MAX_ROUNDS - 1) add("note", "(Stopped after " + MAX_ROUNDS + " rounds of changes.)");
      }
    } catch (e) {
      add("note error", e.name === "AbortError" ? "Stopped." : "Couldn't finish: " + e.message);
    } finally {
      clearInterval(ticker);
      call("tessera_end_group");  // everything this reply changed is one undo step
      job = null;
      setStatus("");
      $("chat-send").disabled = false;
      $("chat-stop").hidden = true;
    }
  }

  // ------------------------------------------------------------ setup

  function showTab(name) {
    for (const b of document.querySelectorAll(".side-tabs button")) b.classList.toggle("on", b.dataset.tab === name);
    $("properties-tab").hidden = name !== "properties";
    $("chat-tab").hidden = name !== "chat";
    if (name === "chat") $("chat-input").focus();
  }

  function init() {
    for (const b of document.querySelectorAll(".side-tabs button")) b.addEventListener("click", () => showTab(b.dataset.tab));
    const input = $("chat-input");
    const submit = () => {
      const text = input.value.trim();
      if (!text || job) return;
      input.value = "";
      send(text);
    };
    $("chat-form").addEventListener("submit", (e) => { e.preventDefault(); submit(); });
    input.addEventListener("keydown", (e) => {
      if (e.key === "Enter" && !e.shiftKey) { e.preventDefault(); submit(); }  // Shift+Enter: a new line
    });
    $("chat-stop").addEventListener("click", () => { if (job) job.controller.abort(); });
    $("chat-new").addEventListener("click", () => {
      if (job) return;
      messages = [];
      $("chat-log").textContent = "";
      add("note", "New chat. Describe a change to the scene.");
    });
    const urlInput = $("chat-url");
    urlInput.value = workerUrl();
    urlInput.addEventListener("change", () => {
      try { localStorage.setItem("tessera-chat-url", urlInput.value.trim()); } catch (e) { /* storage blocked */ }
    });
    add("note", workerUrl()
      ? "Describe a change, like “add three spheres in a row and make them bounce one after another”. Each reply is one undo step."
      : "The chat needs the Tessera Worker (see worker/ in the repository): enter its URL in Settings.");
  }

  return { init: init, send: send, messages: () => messages };
})();
