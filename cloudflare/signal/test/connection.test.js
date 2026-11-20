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

test("temporary C- viewer can be targeted by host SDP offer", () => {
  const host = new Socket("123456789");
  const viewer = new Socket("C-987654321");
  const hub = testHub([host,viewer]);
  assert.equal(hub.socket("C-987654321"), viewer);
  assert.equal(hub.online("C-987654321"), false); // transient peer is not a host
  hub.relay(host,"123456789",{
    type:"offer",transmission_id:"123456789",
    remote_user_id:"C-987654321",user_id:"forged",
    sdp:"v=0\r\n"
  });
  assert.deepEqual(viewer.messages, [{
    type:"offer",transmission_id:"123456789",
    remote_user_id:"123456789",user_id:"123456789",sdp:"v=0\r\n"
  }]);
});

test("temporary C- viewer can answer to its numeric target host", () => {
  const host = new Socket("123456789");
  const viewer = new Socket("C-987654321");
  const hub = testHub([host,viewer]);
  hub.relay(viewer,"C-987654321",{
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
