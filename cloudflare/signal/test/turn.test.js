import test from "node:test";
import assert from "node:assert/strict";
import { SignalHub } from "../src/worker.js";

function hub(env = {}) {
  const value = Object.create(SignalHub.prototype);
  value.env = env;
  return value;
}
const KEY_ID = "a".repeat(32);
const KEY_SECRET = "b".repeat(64);
const cfResponse = {iceServers:[
  {urls:["stun:stun.cloudflare.com:3478"]},
  {urls:[
    "turn:turn.cloudflare.com:3478?transport=udp",
    "turn:turn.cloudflare.com:443?transport=udp",
    "turn:turn.cloudflare.com:3478?transport=tcp",
    "turn:turn.cloudflare.com:80?transport=tcp",
    "turns:turn.cloudflare.com:443?transport=tcp",
    "turn:turn.cloudflare.com:53?transport=udp"
  ],username:"short-lived-user",credential:"short-lived-password"}]};
const originalFetch=globalThis.fetch;
async function withFetch(fn, perform) {
  globalThis.fetch=fn;
  try { return await perform(); }
  finally { globalThis.fetch=originalFetch; }
}
test("unconfigured TURN keeps existing STUN-only P2P", async()=>{
  const config=await hub().connectionIce();
  assert.equal(config.version,1);
  assert.deepEqual(config.servers,[{urls:["stun:stun.cloudflare.com:3478"]}]);
});
test("Cloudflare TURN credentials are short-lived, filtered for MiniRTC, and never expose the key",async()=>{
  const h=hub({TURN_KEY_ID:KEY_ID,TURN_API_TOKEN:KEY_SECRET});
  let count=0;
  const ice=await withFetch(async (url, options)=>{
    count++;
    assert.equal(url,`https://rtc.live.cloudflare.com/v1/turn/keys/${KEY_ID}/credentials/generate-ice-servers`);
    assert.equal(options.method,"POST");
    assert.equal(options.headers.Authorization,`Bearer ${KEY_SECRET}`);
    assert.deepEqual(JSON.parse(options.body),{ttl:21600});
    return {ok:true,json:async()=>cfResponse};
  },()=>h.connectionIce());
  assert.equal(count,1);
  assert.equal(ice.version,1);
  assert.ok(ice.expires_at > Date.now()/1000+21400);
  assert.deepEqual(ice.servers[0],{urls:["stun:stun.cloudflare.com:3478"]});
  assert.deepEqual(ice.servers[1].urls,[
    "turn:turn.cloudflare.com:3478?transport=udp",
    "turn:turn.cloudflare.com:443?transport=udp",
    "turn:turn.cloudflare.com:3478?transport=tcp",
    "turn:turn.cloudflare.com:80?transport=tcp"
  ]);
  assert.equal(ice.servers[1].username,"short-lived-user");
  assert.equal(ice.servers[1].credential,"short-lived-password");
  assert.equal(ice.servers[1].expires_at,ice.expires_at);
  assert.ok(!JSON.stringify(ice).includes(KEY_SECRET));
  assert.ok(!JSON.stringify(ice).includes(KEY_ID));
});
test("Cloudflare failure falls back to STUN without credential leakage",async()=>{
  const h=hub({TURN_KEY_ID:KEY_ID,TURN_API_TOKEN:KEY_SECRET});
  const warnings=[];
  const oldWarn=console.warn;
  console.warn=(...args)=>warnings.push(args);
  try {
    const ice=await withFetch(async()=>({ok:false,status:401}),()=>h.connectionIce());
    assert.equal(ice.servers.length,1);
    assert.equal(warnings[0][0],"redesk.turn");
    assert.ok(!JSON.stringify(warnings).includes(KEY_SECRET));
    assert.ok(!JSON.stringify(warnings).includes(KEY_ID));
  } finally {console.warn=oldWarn;}
});
test("response with only unsupported turns: URLs does not claim TURN is available",async()=>{
  const h=hub({TURN_KEY_ID:KEY_ID,TURN_API_TOKEN:KEY_SECRET});
  const oldWarn=console.warn; console.warn=()=>{};
  try {
    const ice=await withFetch(async()=>({ok:true,json:async()=>({
      iceServers:[{urls:["turns:turn.cloudflare.com:443?transport=tcp"],username:"u",credential:"p"}]
    })}),()=>h.connectionIce());
    assert.equal(ice.servers.length,1);
  } finally {console.warn=oldWarn;}
});
