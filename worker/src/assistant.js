// assistant.js — what the model is told and what it can do. Both are fixed here in
// the Worker (the page can't change them). Each tool is one of Tessera's scene
// commands (src/commands.cpp); the page runs every call through the same validated
// command path the editor UI uses, and sends back the result or the error.

export const SYSTEM_PROMPT = `You are the assistant inside Tessera, a small Blender-style 3D editor that renders on the CPU. The user describes changes to their scene in plain English; you make them with the tools, which are the editor's own scene commands. Each user message starts with the current scene as JSON.

Scene conventions:
- Right-handed, Y up; the ground grid is the plane y = 0. Units are arbitrary (about a meter).
- Primitives (cube, sphere, cylinder, cone, plane) are 2 units across, centered on their origin, so a unit-scale cube resting on the ground sits at y = 1 and a plane lies flat at y = 0.
- An object's position, rotation, and scale are relative to its parent (worldPosition in the scene JSON shows where a child really is). Rotations are XYZ Euler angles in degrees (rotationEuler / eulerDegrees); X is applied first.
- Colors are RGB from 0 to 1.
- Cameras look down their local -Z; a camera with lookAt always faces that object, whatever its rotation. The scene renders and exports through the active camera.
- The sun light shines along its local -Z and casts shadows by default (setLight shadows: false turns them off). Meshes with the liquid or glass shader don't cast or receive shadows. A plane under objects makes their shadows visible.

Meshes: get_mesh lists a mesh object's vertices (positions in the object's own space, before its transform) and faces (vertex index lists, counter-clockwise seen from outside); the scene JSON gives each mesh's faceCount. editMesh runs one modeling operation on chosen vertices, edges (vertex index pairs), or faces. Every editMesh replaces the mesh and can renumber its elements, so take the next call's indices from the previous result (it returns what the operation leaves selected, like the extruded faces) or from a fresh get_mesh; don't run several editMesh calls on one object in parallel. Extrude leaves the new faces in place: follow it with a move of their vertices (get_mesh gives their positions).

Animation: keys are at times in seconds (frame / fps). Use addKeyframe for animation, with eulerDegrees for rotation keys. Once a property has keys, a setTransform/setMaterial/setCamera/setLight on it updates the key at the current playhead frame instead of the base value, so prefer addKeyframe with explicit times when animating. setAnimation sets the frame range and fps.

Working:
- Use the object ids from the scene JSON. addObject returns the new object's id; use it in later calls.
- Make independent calls in parallel. If a call fails, read the error, fix the input, and try again; don't repeat a failing call unchanged.
- Your whole reply is one undo step for the user, so it's fine to make many changes.
- If the request is ambiguous, pick sensible values (sizes, colors, positions that are visible from the active camera) rather than asking, and say what you chose.
- When you're done, reply in a sentence or two saying what you changed. Don't list every call.
- You can only change the scene through these tools. If something is impossible with them (for example importing a file), say so briefly.`;

const vec3 = (description) => ({ type: "array", items: { type: "number" }, minItems: 3, maxItems: 3, description });
const id = { type: "integer", description: "Object id from the scene JSON." };
const object = (properties, required, description) => ({
  type: "object", properties, required, additionalProperties: false, ...(description ? { description } : {}),
});

const material = object({
  color: vec3("RGB, 0-1."),
  specular: { type: "number", description: "Highlight strength, 0-10 (default 0.35)." },
  shininess: { type: "number", description: "Highlight tightness, 1-1024 (default 48)." },
  checker: { type: "boolean", description: "Show a checkerboard texture." },
}, []);
const shader = object({
  type: { type: "string", enum: ["", "liquid", "glass"], description: '"" is the standard shader.' },
  params: { type: "object", description: "liquid: {fillLevel (-10..10, height above the object's center), foamWidth (0..1), layers: [[r,g,b], ...]}. glass: {tint: [r,g,b], rimStart (0..1)}. Empty for the standard shader." },
}, ["type"]);

export const TOOLS = [
  {
    name: "addObject",
    description: "Add an object. A mesh needs a primitive. Returns {id, name}.",
    input_schema: object({
      type: { type: "string", enum: ["mesh", "light", "camera"] },
      name: { type: "string" },
      primitive: object({
        type: { type: "string", enum: ["cube", "sphere", "cylinder", "cone", "plane"] },
        segments: { type: "integer", description: "Around the circle, 3-256 (default 32)." },
        rings: { type: "integer", description: "Sphere rings, 2-256 (default 16)." },
      }, ["type"]),
      parent: { type: "integer", description: "Parent object id (0 or omitted: none)." },
      transform: object({
        position: vec3("Relative to the parent."),
        scale: vec3(),
      }, [], "Position and scale. Set rotation afterwards with setTransform's rotationEuler."),
      material,
      shader,
      light: object({ color: vec3(), intensity: { type: "number", description: "0-100 (default 1)." }, shadows: { type: "boolean", description: "Cast shadows (default true)." } }, []),
      camera: object({
        fov: { type: "number", description: "Vertical field of view in degrees, 1-170 (default 50)." },
        lookAt: { type: "integer", description: "Object id to always face (0: none)." },
      }, []),
      activeCamera: { type: "boolean", description: "For cameras: make it the one the scene renders through." },
    }, ["type"]),
  },
  {
    name: "removeObject",
    description: "Delete an object. It must have no children (re-parent them first with setParent).",
    input_schema: object({ id }, ["id"]),
  },
  {
    name: "setTransform",
    description: "Move, rotate, or scale an object. Fields left out keep their value. Values are relative to the parent.",
    input_schema: object({
      id,
      position: vec3(),
      rotationEuler: vec3("XYZ Euler angles in degrees."),
      scale: vec3(),
    }, ["id"]),
  },
  { name: "setName", description: "Rename an object (a taken name gets a .001 suffix).", input_schema: object({ id, name: { type: "string" } }, ["id", "name"]) },
  { name: "setMaterial", description: "Change a mesh's material.", input_schema: object({ id, ...material.properties }, ["id"]) },
  { name: "setShader", description: "Change a mesh's shader.", input_schema: object({ id, ...shader.properties }, ["id", "type"]) },
  {
    name: "setLight",
    description: "Change a light's color, intensity, or whether it casts shadows.",
    input_schema: object({ id, color: vec3(), intensity: { type: "number", description: "0-100." }, shadows: { type: "boolean", description: "Objects cast shadows from this light." } }, ["id"]),
  },
  {
    name: "setCamera",
    description: "Change a camera's field of view, clipping, or look-at target.",
    input_schema: object({
      id,
      fov: { type: "number", description: "Degrees, 1-170." },
      near: { type: "number" },
      far: { type: "number" },
      lookAt: { type: "integer", description: "Object id to always face (0: none)." },
    }, ["id"]),
  },
  { name: "setActiveCamera", description: "Choose the camera the scene renders through (id 0: none).", input_schema: object({ id }, ["id"]) },
  { name: "setVisible", description: "Show or hide an object.", input_schema: object({ id, visible: { type: "boolean" } }, ["id", "visible"]) },
  {
    name: "setParent",
    description: "Parent an object to another (parent 0: no parent). By default it stays where it is in the world (keepTransform).",
    input_schema: object({ id, parent: { type: "integer" }, keepTransform: { type: "boolean" } }, ["id", "parent"]),
  },
  {
    name: "setWorld",
    description: "Change the background color and the ambient light level.",
    input_schema: object({ ambient: { type: "number", description: "0-1." }, background: vec3() }, []),
  },
  {
    name: "setAnimation",
    description: "Set the animation's frame rate and frame range.",
    input_schema: object({ fps: { type: "number", description: "1-240." }, start: { type: "integer" }, end: { type: "integer" } }, []),
  },
  {
    name: "addKeyframe",
    description: "Key a property at a time (replacing a key already there). The value blends into the next key with this key's easing.",
    input_schema: object({
      id,
      property: { type: "string", enum: ["position", "rotation", "scale", "color", "intensity", "fov"] },
      time: { type: "number", description: "Seconds (frame / fps)." },
      value: { type: "array", items: { type: "number" }, description: "position/scale/color: 3 numbers; intensity/fov: 1 number. For rotation use eulerDegrees instead." },
      eulerDegrees: vec3("Rotation keys only: XYZ Euler angles in degrees."),
      easing: { type: "string", enum: ["linear", "easeIn", "easeOut", "easeInOut", "step"] },
    }, ["id", "property", "time"]),
  },
  {
    name: "removeKeyframe",
    description: "Delete the key of a property at a time.",
    input_schema: object({ id, property: { type: "string" }, time: { type: "number", description: "Seconds." } }, ["id", "property", "time"]),
  },
  {
    name: "moveKeyframe",
    description: "Move a key to another time.",
    input_schema: object({ id, property: { type: "string" }, time: { type: "number" }, newTime: { type: "number" } }, ["id", "property", "time", "newTime"]),
  },
  {
    name: "editMesh",
    description: "Run one modeling operation on a mesh object (indices from get_mesh). Returns {mesh, verts, edges, faces}: the new mesh's id and what the operation leaves selected (the extruded faces, the inner faces of an inset, the new edge loop...), as indices into the new mesh.",
    input_schema: object({
      id,
      operation: {
        type: "string", enum: ["extrude", "inset", "bevel", "loopcut", "subdivide", "merge", "delete", "move"],
        description: "extrude: the faces as one region (or, without faces, open boundary edges), new geometry left in place; inset: each face shrinks inward by amount, ringed by new quads; bevel: edges (inside a closed surface) become flat strips amount wide; loopcut: cuts evenly spaced around the ring of quads through the first edge; subdivide: each face split into 4, cuts times; merge: the vertices become one at their center; delete: the verts, edges, or faces (what) and the faces using them; move: the verts to positions.",
      },
      verts: { type: "array", items: { type: "integer" }, description: "Vertex indices (merge, move, delete)." },
      edges: { type: "array", items: { type: "array", items: { type: "integer" }, minItems: 2, maxItems: 2 }, description: "Edges as vertex index pairs (bevel, loopcut, extrude, delete)." },
      faces: { type: "array", items: { type: "integer" }, description: "Face indices (extrude, inset, subdivide, delete)." },
      amount: { type: "number", description: "inset and bevel: distance in the mesh's own units (default 0.1)." },
      cuts: { type: "integer", description: "loopcut: number of cuts; subdivide: levels, 1-4 (default 1)." },
      what: { type: "string", enum: ["verts", "edges", "faces"], description: "delete: which kind of element to delete." },
      positions: { type: "array", items: vec3(), description: "move: one new position (the object's own space) per vertex in verts, in the same order." },
    }, ["id", "operation"]),
  },
  {
    name: "get_mesh",
    description: "Read a mesh object's vertices (positions in its own space) and faces (vertex index lists). Very large meshes give only counts.",
    input_schema: object({ id }, ["id"]),
  },
  {
    name: "get_scene",
    description: "Read the scene again (the same JSON as at the start of the message), after your changes.",
    input_schema: object({}, []),
  },
];
