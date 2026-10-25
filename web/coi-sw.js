// coi-sw.js — makes the page "cross-origin isolated", which browsers require before
// they allow SharedArrayBuffer, and so WebAssembly threads. Isolation takes two
// response headers that GitHub Pages can't be configured to send:
//   Cross-Origin-Opener-Policy: same-origin     (no other site's window shares our process)
//   Cross-Origin-Embedder-Policy: require-corp  (nothing from another site loads without its consent)
// A service worker sits between the page and the network for this site, so it can add
// them to every response. The page registers it on the first visit and reloads once
// (see shell.html). Requests to other sites, like the AI chat's Worker, pass through
// untouched: under require-corp they still work, because they're CORS requests.
"use strict";

self.addEventListener("install", () => self.skipWaiting());  // take over without waiting for old tabs to close
self.addEventListener("activate", (event) => event.waitUntil(self.clients.claim()));

self.addEventListener("fetch", (event) => {
  const request = event.request;
  if (new URL(request.url).origin !== self.location.origin) return;
  // A cache-only request can't be made from a worker for anything but same-origin mode.
  if (request.cache === "only-if-cached" && request.mode !== "same-origin") return;
  event.respondWith(fetch(request).then((response) => {
    if (response.status === 0) return response;  // opaque: headers can't be changed
    const headers = new Headers(response.headers);
    headers.set("Cross-Origin-Opener-Policy", "same-origin");
    headers.set("Cross-Origin-Embedder-Policy", "require-corp");
    return new Response(response.body, { status: response.status, statusText: response.statusText, headers });
  }));
});
