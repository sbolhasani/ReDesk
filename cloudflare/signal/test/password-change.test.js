import test from "node:test";
import assert from "node:assert/strict";
import { webcrypto } from "node:crypto";
import { SignalHub } from "../src/worker.js";

const crypto = globalThis.crypto ?? webcrypto;
const digest = async (salt, password) => Buffer.from(
  await crypto.subtle.digest("SHA-256", new TextEncoder().encode(salt + ":" + password))
).toString("hex");
class Socket {
  constructor(user) { this.state={phase:"authenticated",user,subscriptions:[]};this.messages=[]; }
  deserializeAttachment() { return structuredClone(this.state); }
  serializeAttachment(value) { this.state=structuredClone(value); }
  send(raw) { this.messages.push(JSON.parse(raw)); }
}
function setup() {
  const sockets = [];
  const users = new Map();
  const sql = {exec(statement,...args) {
    if (statement.startsWith("SELECT id, public_key, salt, hash FROM devices")) {
      const row=users.get(args[0]);return {toArray:()=>row?[row]:[]};
    }
    if (statement.startsWith("UPDATE devices SET salt=?, hash=? WHERE id=? AND public_key=?")) {
      const [salt,hash,id,key]=args;
      const row=users.get(id);
      if (row?.public_key===key) users.set(id,{...row,salt,hash});
      return {toArray:()=>[]};
    }
    throw new Error("Unsupported SQL "+statement);
  }};
  const hub=Object.create(SignalHub.prototype);
  hub.ctx={getWebSockets:()=>sockets,storage:{sql}};
  hub.env={};
  return {hub,sockets,users};
}
test("authenticated primary device rotates password and receives matching request_id", async()=>{
  const {hub,sockets,users}=setup();
  users.set("123456789",{id:"123456789",public_key:"ab".repeat(32),salt:"old",hash:await digest("old","123456")});
  const primary=new Socket("123456789");sockets.push(primary);
  await hub.webSocketMessage(primary,JSON.stringify({
    type:"change_password",request_id:"change-123",new_password:"AB12cd"
  }));
  assert.deepEqual(primary.messages.at(-1),{type:"change_password",status:"success",user_id:"123456789",request_id:"change-123"});
  const {salt,hash}=users.get("123456789");
  assert.equal(hash,await digest(salt,"AB12cd"));
  assert.notEqual(hash,await digest(salt,"123456"));
});
test("viewer, unauthorized and invalid requests never rotate password", async()=>{
  const {hub,sockets,users}=setup();
  const initial={id:"123456789",public_key:"ab".repeat(32),salt:"old",hash:await digest("old","123456")};
  users.set("123456789",{...initial});
  const viewer=new Socket("C-123456789");
  const primary=new Socket("123456789");sockets.push(primary,viewer);
  for (const request of [
    {socket:viewer,password:"XX1234",request_id:"viewer",reason:"not_authorized"},
    {socket:primary,password:"!oops!",request_id:"invalid",reason:"invalid_request"},
    {socket:primary,password:"1234",request_id:"short",reason:"invalid_request"}
  ]) {
    await hub.webSocketMessage(request.socket,JSON.stringify({
      type:"change_password",request_id:request.request_id,new_password:request.password
    }));
    assert.equal(request.socket.messages.at(-1).reason,request.reason);
    assert.deepEqual(users.get("123456789"),initial);
  }
});
test("missing account cannot be silently created by password change",async()=>{
  const {hub,sockets,users}=setup();
  const primary=new Socket("123456789");sockets.push(primary);
  await hub.webSocketMessage(primary,JSON.stringify({
    type:"change_password",request_id:"absent",new_password:"Ab1234"
  }));
  assert.equal(primary.messages.at(-1).reason,"unknown_id");
  assert.equal(users.size,0);
});
