// index.js — the Cloudflare Worker between Tessera's chat panel and the Claude API.
// It holds the API key (a Worker secret: never in the page), and fixes the model, the
// instructions, and the tools; the page only sends the conversation. One request is
// one model turn: the page runs the tool calls in the editor and sends the results
// back as the next request, so the agent loop lives in the page and the key here.
//
// The site is public, so the Worker guards the key with limits: only the allowed
// origins, a per-visitor rate limit, a cap on request size and conversation length,
// and a cap on each reply. (A spend limit on the key's workspace in the Anthropic
// Console is the real ceiling on the bill.)
import Anthropic from "@anthropic-ai/sdk";
import { SYSTEM_PROMPT, TOOLS } from "./assistant.js";

const MAX_BODY_BYTES = 400_000;  // a long chat with scene snapshots fits; more means start a new chat
const MAX_MESSAGES = 80;
const MAX_REPLY_TOKENS = 8000;   // a reply is a few tool calls and a sentence or two

function corsHeaders(origin) {
  return {
    "Access-Control-Allow-Origin": origin,
    "Access-Control-Allow-Methods": "POST, OPTIONS",
    "Access-Control-Allow-Headers": "Content-Type",
    "Access-Control-Max-Age": "86400",
    Vary: "Origin",
  };
}

function reply(status, body, origin) {
  return new Response(JSON.stringify(body), {
    status,
    headers: { "Content-Type": "application/json", ...(origin ? corsHeaders(origin) : {}) },
  });
}

// The conversation must be ordinary user/assistant turns. The page can't add system
// messages (operator instructions) or change anything but the messages.
function checkMessages(messages) {
  if (!Array.isArray(messages) || messages.length === 0) return "messages must be a non-empty array";
  if (messages.length > MAX_MESSAGES) return "the conversation is too long: start a new chat";
  if (messages[0].role !== "user") return "the conversation must start with the user";
  for (const m of messages) {
    if (!m || (m.role !== "user" && m.role !== "assistant")) return "only user and assistant messages are allowed";
    if (typeof m.content !== "string" && !Array.isArray(m.content)) return "message content must be text or blocks";
  }
  return null;
}

export default {
  async fetch(request, env) {
    const origin = request.headers.get("Origin") || "";
    const allowed = (env.ALLOWED_ORIGINS || "").split(",").map((s) => s.trim()).filter(Boolean);
    const originOk = allowed.includes(origin);
    const url = new URL(request.url);

    if (request.method === "OPTIONS") {  // CORS preflight
      return originOk ? new Response(null, { status: 204, headers: corsHeaders(origin) }) : new Response(null, { status: 403 });
    }
    if (url.pathname !== "/chat" || request.method !== "POST") return reply(404, { error: "not found" });
    if (!originOk) return reply(403, { error: "this Worker only answers Tessera's own pages" });

    const visitor = request.headers.get("CF-Connecting-IP") || "unknown";
    if (env.CHAT_LIMITER) {
      const { success } = await env.CHAT_LIMITER.limit({ key: visitor });
      if (!success) return reply(429, { error: "too many requests: wait a minute and try again" }, origin);
    }

    const text = await request.text();
    if (text.length > MAX_BODY_BYTES) return reply(413, { error: "the conversation is too long: start a new chat" }, origin);
    let body;
    try {
      body = JSON.parse(text);
    } catch {
      return reply(400, { error: "the request isn't JSON" }, origin);
    }
    const problem = checkMessages(body && body.messages);
    if (problem) return reply(400, { error: problem }, origin);
    if (!env.ANTHROPIC_API_KEY) return reply(500, { error: "the Worker has no ANTHROPIC_API_KEY secret" }, origin);

    const client = new Anthropic({ apiKey: env.ANTHROPIC_API_KEY });
    try {
      const response = await client.beta.messages.create({
        model: env.MODEL || "claude-opus-5-5",
        max_tokens: MAX_REPLY_TOKENS,
        system: SYSTEM_PROMPT,
        tools: TOOLS,
        messages: body.messages,
        output_config: { effort: "medium" },
        cache_control: { type: "ephemeral" },  // the instructions and tools repeat every turn
        // If a safety classifier declines, retry on the model Anthropic recommends.
        betas: ["server-side-fallback-2026-07-01"],
        fallbacks: "default",
      });
      // Returned whole: the page appends the content unchanged to the conversation.
      return reply(200, response, origin);
    } catch (error) {
      if (error instanceof Anthropic.RateLimitError) return reply(429, { error: "Claude is busy: try again in a moment" }, origin);
      if (error instanceof Anthropic.BadRequestError) return reply(400, { error: "Claude rejected the request: " + error.message }, origin);
      if (error instanceof Anthropic.AuthenticationError) return reply(500, { error: "the Worker's API key isn't valid" }, origin);
      if (error instanceof Anthropic.APIError) return reply(502, { error: "Claude API error " + (error.status || "") }, origin);
      return reply(502, { error: "couldn't reach Claude" }, origin);
    }
  },
};
