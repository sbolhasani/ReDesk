import test from "node:test";
import assert from "node:assert/strict";
import worker from "../src/worker.js";

// Run with "npm run check" before deploying. These smoke tests do not
// substitute for wrangler's Durable Object/WebSocket integration tests.
test("health response does not need Durable Object binding", async () => {
  const response = await worker.fetch(new Request("https://local.example/health"), {});
  assert.equal(response.status, 200);
  assert.equal((await response.json()).service, "ReDesk signaling");
});
test("non-websocket signaling request is rejected", async () => {
  const response = await worker.fetch(new Request("https://local.example/signal"), {});
  assert.equal(response.status, 404);
});
test("unknown URLs do not disclose backend internals", async () => {
  const response = await worker.fetch(new Request("https://local.example/admin"), {});
  assert.equal(response.status, 404);
});
