import test from "node:test";
import assert from "node:assert/strict";
import { SignalHub } from "../src/worker.js";

// Pure in-memory Durable Object/WebSocket test double. These tests exercise
// actual Worker logic without Cloudflare credentials or a media transport.
class Socket {
  constructor(state) { this.state = structuredClone(state); this.messages = []; }
  deserializeAttachment() { return structuredClone(this.state); }
  serializeAttachment(state) { this.state = structuredClone(state); }
  send(payload) { this.messages.push(JSON.parse(payload)); }
  close() { this.closed = true; }
}
function hub(sockets, env = {}) {
  const value = Object.create(SignalHub.prototype);
  value.ctx = {getWebSockets: () => sockets};
  value.env = env;
  return value;
}
function authenticated(id, subscriptions = []) {
  return new Socket({phase:"authenticated", user:id, subscriptions});
}

test("two authenticated devices see each other online even without readyState", async () => {
  const one = authenticated("123456789");
  const two = authenticated("987654321");
  const instance = hub([one, two]);
  assert.equal(instance.online("123456789"), true);
  assert.equal(instance.online("987654321"), true);
  await instance.webSocketMessage(one, JSON.stringify({
    type:"recent_connections_presence", user_id:"123456789",
    devices:["987654321"], subscribe:false
  }));
  assert.deepEqual(one.messages.at(-1), {
    type:"presence",devices:[{id:"987654321",online:true}]
  });
  assert.equal(one.state.subscriptions.length, 0);
});

test("a one-shot probe does not overwrite persistent presence subscriptions", async () => {
  const one=authenticated("123456789",["987654321"]);
  const two=authenticated("987654321");
  const instance=hub([one,two]);
  await instance.webSocketMessage(one, JSON.stringify({
    type:"recent_connections_presence", user_id:"123456789",
    devices:["333333333"],subscribe:false
  }));
  assert.deepEqual(one.state.subscriptions,["987654321"]);
  assert.deepEqual(one.messages.at(-1).devices,[{id:"333333333",online:false}]);
  instance.updatePresence("987654321",false);
  assert.deepEqual(one.messages.at(-1),{type:"presence_update",id:"987654321",online:false});
});

test("only authenticated sockets are considered online", () => {
  const signedIn=authenticated("123456789");
  const pending=new Socket({phase:"challenge",user:null});
  const instance=hub([signedIn,pending]);
  assert.equal(instance.online("123456789"),true);
  assert.equal(instance.online("987654321"),false);
  assert.equal(instance.online("invalid"),false);
});

test("subscription request replaces the persistent subscription set", async () => {
  const one=authenticated("123456789",["999999999"]);
  const two=authenticated("987654321");
  const instance=hub([one,two]);
  await instance.webSocketMessage(one,JSON.stringify({
    type:"recent_connections_presence",user_id:"123456789",
    devices:["987654321"],subscribe:true
  }));
  assert.deepEqual(one.state.subscriptions,["987654321"]);
  assert.deepEqual(one.messages.at(-1).devices,[{id:"987654321",online:true}]);
});
