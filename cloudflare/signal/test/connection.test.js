import test from "node:test";
import assert from "node:assert/strict";
import { SignalHub } from "../src/worker.js";

class Socket {
  constructor(user) {
    this.state = { phase:"authenticated", user, subscriptions:[] };
    this.messages = [];
  }
  deserializeAttachment() { return structuredClone(this.state); }
  serializeAttachment(value) { this.state=structuredClone(value); }
  send(raw) { this.messages.push(JSON.parse(raw)); }
  close() { this.closed = true; }
}
function testHub(sockets) {
  const hub = Object.create(SignalHub.prototype);
  hub.ctx = {
    getWebSockets() { return sockets; },
    storage: {sql: {
      exec(query, ...args) {
        if (/SELECT token FROM sessions/.test(query)) {
          return {toArray: () => [{token:"session-token"}]};
        }
        if (/SELECT .* FROM devices/.test(query)) {
          return {toArray: () => []};
        }
        return {toArray: () => []};
      }
    }}
  };
  hub.env = {};
  return hub;
}

test("temporary C- viewer can be targeted by host SDP offer", async () => {
  const host = new Socket("123456789");
  const viewer = new Socket("C-987654321");
  const hub = testHub([host,viewer]);
  assert.equal(hub.socket("C-987654321"), viewer);
  assert.equal(hub.online("C-987654321"), false); // transient peer is not a host
  await hub.relay(host,"123456789",{
    type:"offer",transmission_id:"123456789",
    remote_user_id:"C-987654321",user_id:"forged",
    sdp:"v=0\r\n"
  });
  assert.equal(viewer.messages.length,1);
  const offer=viewer.messages[0];
  assert.equal(offer.type,"offer");
  assert.equal(offer.transmission_id,"123456789");
  assert.equal(offer.remote_user_id,"123456789");
  assert.equal(offer.user_id,"123456789");
  assert.equal(offer.sdp,"v=0\r\n");
  // CrossDesk adds AddConnectionIceConfig to the OFFER for the answerer.
  // MiniRTC rejects an offer with no ICE config after ice_config_version:1.
  assert.equal(offer.ice.version,1);
  assert.equal(typeof offer.ice.id,"string");
  assert.equal(offer.ice.id.length,36);
  assert.ok(offer.ice.expires_at > Math.floor(Date.now()/1000));
  assert.deepEqual(offer.ice.servers,[{urls:["stun:stun.cloudflare.com:3478"]}]);
});

test("temporary C- viewer can answer to its numeric target host", async () => {
  const host = new Socket("123456789");
  const viewer = new Socket("C-987654321");
  const hub = testHub([host,viewer]);
  await hub.relay(viewer,"C-987654321",{
    type:"answer",transmission_id:"123456789",
    remote_user_id:"123456789",user_id:"forged",sdp:"v=0\r\n"
  });
  assert.deepEqual(host.messages[0],{
    type:"answer",transmission_id:"123456789",
    remote_user_id:"C-987654321",user_id:"C-987654321",sdp:"v=0\r\n"
  });
});

test("authentication key owner ID uses numeric parent for temporary C- peer", async () => {
  const socket = new Socket(null);
  socket.state = {phase:"new",at:Date.now(),user:null};
  const hub = testHub([socket]);
  hub.account = id => id === "123456789" ? {id, public_key:"a".repeat(64)} : null;
  hub.throttled = () => false;
  await hub.login(socket,socket.state,{
    type:"login",user_id:"C-123456789",
    device_key:{version:1,public_key:"a".repeat(64)}
  });
  assert.equal(socket.messages[0].type,"login_challenge");
  assert.equal(socket.messages[0].user_id,"C-123456789");
  assert.equal(socket.messages[0].password_required,false);
});
