// export.js — rendering out from the page: a PNG of the current frame, and the
// animation as a video. Frames come from C++ (Editor::renderExportFrame), which draws
// frame k at exactly start + k / fps with its own liquid simulation, so a scene
// always renders the same frames. They're encoded with WebCodecs and packed into a
// WebM file by the small muxer below; browsers without WebCodecs fall back to
// MediaRecorder, which records in real time.
"use strict";

var TesseraExport = (function () {
  const $ = (id) => document.getElementById(id);
  const state = () => JSON.parse(Module.ccall("tessera_state", "string", [], []));
  const nextTick = () => new Promise((resolve) => setTimeout(resolve, 0));

  function download(blob, name) {
    const a = document.createElement("a");
    a.href = URL.createObjectURL(blob);
    a.download = name;
    a.click();
    setTimeout(() => URL.revokeObjectURL(a.href), 1000);
  }

  // ------------------------------------------------------------ WebM muxer

  // WebM is Matroska: a tree of EBML elements, each an ID, a size, and a payload.
  // A file is an EBML header, then a Segment holding Info (timescale, duration),
  // Tracks (one video track: codec and size), and Clusters. A Cluster has a start
  // time and SimpleBlocks: one encoded frame each, timed relative to the cluster.
  const enc = new TextEncoder();
  const idBytes = (id) => {
    const out = [];
    for (let v = id; v > 0; v = Math.floor(v / 256)) out.unshift(v & 0xFF);
    return new Uint8Array(out);
  };
  // Sizes as 8-byte EBML numbers: a 0x01 marker byte, then 7 bytes big-endian.
  const sizeBytes = (n) => {
    const out = new Uint8Array(8);
    out[0] = 0x01;
    for (let i = 7; i >= 1; --i, n = Math.floor(n / 256)) out[i] = n & 0xFF;
    return out;
  };
  const length = (parts) => parts.reduce((sum, p) => sum + p.length, 0);
  // An element: its parts (header first) as a flat list, ready for a Blob.
  const element = (id, ...payload) => {
    const parts = payload.flat();
    return [idBytes(id), sizeBytes(length(parts)), ...parts];
  };
  const uint = (id, n) => {
    const out = [];
    do { out.unshift(n & 0xFF); n = Math.floor(n / 256); } while (n > 0);
    return element(id, new Uint8Array(out));
  };
  const float = (id, x) => {
    const b = new Uint8Array(8);
    new DataView(b.buffer).setFloat64(0, x);
    return element(id, b);
  };
  const string = (id, s) => element(id, enc.encode(s));

  class WebMMuxer {
    constructor(width, height, codecId) {
      Object.assign(this, { width, height, codecId, clusters: [], current: null, endMs: 0 });
    }

    add(chunk) {  // an EncodedVideoChunk, in order
      const ms = Math.round(chunk.timestamp / 1000);
      const key = chunk.type === "key";
      // A new cluster at every keyframe (so players can seek to it), and before a
      // block's 16-bit relative time could overflow.
      if (!this.current || key || ms - this.current.startMs > 30000) {
        this.current = { startMs: ms, blocks: [] };
        this.clusters.push(this.current);
      }
      const data = new Uint8Array(chunk.byteLength);
      chunk.copyTo(data);
      const rel = ms - this.current.startMs;
      // Track 1 (as an EBML number: 0x81), the time relative to the cluster, flags.
      const header = new Uint8Array([0x81, (rel >> 8) & 0xFF, rel & 0xFF, key ? 0x80 : 0]);
      this.current.blocks.push(element(0xA3, header, data));
      this.endMs = Math.max(this.endMs, ms + Math.round((chunk.duration || 0) / 1000));
    }

    finish() {
      const header = element(0x1A45DFA3,
        uint(0x4286, 1), uint(0x42F7, 1), uint(0x42F2, 4), uint(0x42F3, 8),  // EBML versions, max ID and size lengths
        string(0x4282, "webm"), uint(0x4287, 2), uint(0x4285, 2));          // doc type and its versions
      const info = element(0x1549A966,
        uint(0x2AD7B1, 1000000),          // timestamps count milliseconds (in nanoseconds)
        string(0x4D80, "Tessera"), string(0x5741, "Tessera"),
        float(0x4489, this.endMs));       // duration, in those milliseconds
      const tracks = element(0x1654AE6B, element(0xAE,
        uint(0xD7, 1), uint(0x73C5, 1), uint(0x83, 1),  // track number, UID, type (1: video)
        string(0x86, this.codecId),
        element(0xE0, uint(0xB0, this.width), uint(0xBA, this.height))));
      const clusters = this.clusters.map((c) => element(0x1F43B675, uint(0xE7, c.startMs), ...c.blocks));
      const segment = element(0x18538067, info, tracks, ...clusters);
      return new Blob([...header, ...segment], { type: "video/webm" });
    }
  }

  // ------------------------------------------------------------ video sinks

  // Both take frames as BGRA bytes (the framebuffer's 0xAARRGGBB, little-endian).
  async function webCodecsSink(width, height, fps) {
    if (!("VideoEncoder" in window)) return null;
    for (const [codec, id, label] of [["vp09.00.10.08", "V_VP9", "VP9"], ["vp8", "V_VP8", "VP8"]]) {
      const config = { codec, width, height, framerate: fps, bitrate: Math.round(width * height * fps * 0.2) };
      const support = await VideoEncoder.isConfigSupported(config).catch(() => null);
      if (!support || !support.supported) continue;
      const muxer = new WebMMuxer(width, height, id);
      let failure = null;
      const encoder = new VideoEncoder({ output: (chunk) => muxer.add(chunk), error: (e) => { failure = e; } });
      encoder.configure(config);
      const keyEvery = Math.max(1, Math.round(fps * 2));  // a keyframe every 2 seconds
      return {
        description: "WebM video (" + label + ", WebCodecs)",
        extension: "webm",
        async add(bgra, index) {
          if (failure) throw failure;
          const frame = new VideoFrame(bgra, {
            format: "BGRA", codedWidth: width, codedHeight: height,
            timestamp: Math.round(index * 1e6 / fps), duration: Math.round(1e6 / fps),
          });
          encoder.encode(frame, { keyFrame: index % keyEvery === 0 });
          frame.close();
          while (encoder.encodeQueueSize > 2) await nextTick();  // don't run ahead of the encoder
        },
        async finish() {
          await encoder.flush();
          encoder.close();
          if (failure) throw failure;
          return muxer.finish();
        },
        abort() { try { encoder.close(); } catch (e) { /* already closed */ } },
      };
    }
    return null;
  }

  // Without WebCodecs: draw each frame to a canvas and let MediaRecorder record the
  // canvas. It timestamps frames as they arrive, so frames are paced in real time
  // (a frame that takes longer than 1/fps to render stretches that moment).
  function mediaRecorderSink(width, height, fps) {
    if (!("MediaRecorder" in window)) return null;
    const type = ["video/webm;codecs=vp9", "video/webm", "video/mp4"].find((t) => MediaRecorder.isTypeSupported(t));
    if (!type) return null;
    const canvas = Object.assign(document.createElement("canvas"), { width, height });
    const ctx = canvas.getContext("2d");
    const stream = canvas.captureStream(0);
    const track = stream.getVideoTracks()[0];
    const recorder = new MediaRecorder(stream, { mimeType: type, videoBitsPerSecond: Math.round(width * height * fps * 0.2) });
    const parts = [];
    recorder.ondataavailable = (e) => { if (e.data.size) parts.push(e.data); };
    recorder.start();
    const image = ctx.createImageData(width, height);
    let due = performance.now();
    return {
      description: "Video via MediaRecorder (" + type.split(";")[0] + ", recorded in real time)",
      extension: type.includes("mp4") ? "mp4" : "webm",
      async add(bgra) {
        const px = image.data;
        for (let i = 0; i < px.length; i += 4) {  // BGRA -> RGBA
          px[i] = bgra[i + 2];
          px[i + 1] = bgra[i + 1];
          px[i + 2] = bgra[i];
          px[i + 3] = 255;
        }
        ctx.putImageData(image, 0, 0);
        track.requestFrame();
        due += 1000 / fps;
        await new Promise((resolve) => setTimeout(resolve, Math.max(0, due - performance.now())));
      },
      finish: () => new Promise((resolve) => {
        recorder.onstop = () => resolve(new Blob(parts, { type: type.split(";")[0] }));
        recorder.stop();
      }),
      abort() { recorder.stop(); },
    };
  }

  // ------------------------------------------------------------ the Render dialog

  let mode = "animation";  // or "image"
  let running = null;      // {cancelled} while rendering

  function sizeOption() {
    const [w, h] = $("render-size").value.split("x").map(Number);
    return { width: w, height: h };
  }

  function open(which) {
    mode = which;
    const s = state();
    const anim = which === "animation";
    $("render-title").textContent = anim ? "Render animation" : "Render image";
    for (const el of document.querySelectorAll("#render-dialog .anim-only")) el.hidden = !anim;
    $("render-fps").value = s.animation.fps;
    $("render-start").value = s.animation.start;
    $("render-end").value = s.animation.end;
    $("render-format").textContent = !anim ? "" :
      "VideoEncoder" in window ? "WebM video (WebCodecs)" : "MediaRecorder fallback: records in real time";
    $("render-progress").hidden = true;
    $("render-note").textContent = s.activeCamera ? "Renders through the active camera, without the editor's overlays."
      : "There's no active camera: " + (anim ? "add one first (Add > Camera)." : "the image uses the viewport's view.");
    $("render-go").disabled = anim && !s.activeCamera;
    $("render-go").textContent = "Render";
    $("render-dialog").showModal();
  }

  function renderImage() {
    const { width, height } = sizeOption();
    const n = Module.ccall("tessera_render_png", "number", ["number", "number"], [width, height]);
    if (!n) return note("Couldn't render at that size.", true);
    const ptr = Module.ccall("tessera_png_data", "number", [], []);
    download(new Blob([Module.HEAPU8.slice(ptr, ptr + n)], { type: "image/png" }), "tessera.png");
    $("render-dialog").close();
  }

  function note(text, isError) {
    $("render-note").textContent = text;
    $("render-note").classList.toggle("error", !!isError);
  }

  async function renderAnimation() {
    const { width, height } = sizeOption();
    const s = state(), a = s.animation;
    const fps = Math.min(240, Math.max(1, Number($("render-fps").value) || a.fps));
    const first = Math.max(0, Math.round(Number($("render-start").value)));
    const last = Math.max(first, Math.round(Number($("render-end").value)));
    // The frame range in seconds (scene frames), sampled at the output rate.
    const start = first / a.fps, end = last / a.fps;
    const count = Math.round((end - start) * fps) + 1;

    const begun = JSON.parse(Module.ccall("tessera_export_begin", "string", ["number", "number"], [width, height]));
    if (!begun.ok) return note(begun.error, true);
    const job = (running = { cancelled: false });
    $("render-go").disabled = true;
    $("render-cancel").textContent = "Stop";
    const bar = $("render-progress");
    bar.hidden = false;
    bar.max = count;
    bar.value = 0;
    let sink = null;
    try {
      sink = (await webCodecsSink(width, height, fps)) || mediaRecorderSink(width, height, fps);
      if (!sink) throw new Error("this browser can't encode video (no WebCodecs or MediaRecorder)");
      const t0 = performance.now();
      for (let k = 0; k < count && !job.cancelled; ++k) {
        const ptr = Module.ccall("tessera_export_frame", "number", ["number", "number"], [start + k / fps, 1 / fps]);
        await sink.add(Module.HEAPU8.subarray(ptr, ptr + width * height * 4), k);
        bar.value = k + 1;
        note("Frame " + (k + 1) + " of " + count + " · " + Math.round((performance.now() - t0) / (k + 1)) + " ms each · " + sink.description);
        await nextTick();  // let the page draw the progress, and see Stop
      }
      if (job.cancelled) {
        sink.abort();
        note("Stopped. Nothing was saved.");
      } else {
        note("Finishing the video…");
        const blob = await sink.finish();
        download(blob, "tessera." + sink.extension);
        note("Saved tessera." + sink.extension + " (" + count + " frames, " + (Math.round(blob.size / 10485.76) / 100) + " MB).");
      }
    } catch (e) {
      if (sink) sink.abort();
      note("Rendering failed: " + (e && e.message ? e.message : e), true);
    } finally {
      Module.ccall("tessera_export_end", null, [], []);
      running = null;
      $("render-go").disabled = false;
      $("render-cancel").textContent = "Close";
    }
  }

  function init() {
    $("render-go").addEventListener("click", (e) => {
      e.preventDefault();
      note("");
      mode === "image" ? renderImage() : renderAnimation();
    });
    $("render-cancel").addEventListener("click", () => {
      if (running) running.cancelled = true;
      else $("render-dialog").close();
    });
    // Escape: stop a render in progress rather than closing over it.
    $("render-dialog").addEventListener("cancel", (e) => {
      if (running) {
        e.preventDefault();
        running.cancelled = true;
      }
    });
  }

  return { init: init, open: open, WebMMuxer: WebMMuxer, isRendering: () => running !== null };
})();
