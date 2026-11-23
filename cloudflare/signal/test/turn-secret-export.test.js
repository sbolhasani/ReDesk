import test from "node:test";
import assert from "node:assert/strict";
import worker from "../src/worker.js";

const path = "https://local.example/__redesk_diag/turn-secret-export";
const key = "a".repeat(32);
const token = "b".repeat(64);
function grant(offset) {
  return String(Math.floor(Date.now() / 1000) + offset) + ":" + "c".repeat(64);
}
function req(auth) {
  return new Request(path, {method:"POST", ...(auth ? {headers:{Authorization:"Bearer " + auth}} : {})});
}
test("turn-secret-export is invisible without an explicit grant", async () => {
  const res = await worker.fetch(req(), {TURN_KEY_ID:key, TURN_API_TOKEN:token});
  assert.equal(res.status, 404);
});
test("turn-secret-export rejects missing, wrong and expired credentials", async () => {
  const value = grant(300);
  const env = {TURN_KEY_ID:key, TURN_API_TOKEN:token, TURN_SECRET_EXPORT_GRANT:value};
  assert.equal((await worker.fetch(req(), env)).status, 404);
  assert.equal((await worker.fetch(req("wrong"), env)).status, 404);
  assert.equal((await worker.fetch(req(value), {...env,TURN_SECRET_EXPORT_GRANT:grant(-1)})).status, 404);
  assert.equal((await worker.fetch(req(value), {...env,TURN_SECRET_EXPORT_GRANT:grant(601)})).status, 404);
});
test("turn-secret-export requires POST and does not expose secret without auth", async () => {
  const value=grant(300);
  const env={TURN_KEY_ID:key,TURN_API_TOKEN:token,TURN_SECRET_EXPORT_GRANT:value};
  assert.equal((await worker.fetch(new Request(path),env)).status,404);
});
test("authorized turn-secret-export retrieves only existing secrets with no-store cache", async () => {
  const value=grant(300);
  const env={TURN_KEY_ID:key,TURN_API_TOKEN:token,TURN_SECRET_EXPORT_GRANT:value};
  const res=await worker.fetch(req(value),env);
  assert.equal(res.status,200);
  assert.equal(res.headers.get("cache-control"),"no-store, private");
  assert.deepEqual(await res.json(),{turn_key_id:key,turn_api_token:token});
  assert.equal((await worker.fetch(req(value),{...env,TURN_API_TOKEN:""})).status,503);
});
