import test from "node:test";
import assert from "node:assert/strict";
import { webcrypto } from "node:crypto";
import { SignalHub } from "../src/worker.js";

const crypto = globalThis.crypto ?? webcrypto;
const encode = new TextEncoder();
const hex = bytes => Buffer.from(bytes).toString("hex");
const seconds = () => Math.floor(Date.now() / 1000);

class Socket {
  constructor() { this.state = {phase:"new",at:Date.now(),user:null};this.messages=[]; }
  deserializeAttachment() { return structuredClone(this.state); }
  serializeAttachment(state) { this.state=structuredClone(state); }
  send(data) { this.messages.push(JSON.parse(data)); }
  close() { this.closed=true; }
}
function fixture() {
  const records=new Map(), loginThrottle=new Map(), joinThrottle=new Map();
  const sockets=[];
  const sql={exec(query,...args) {
    const isLogin=query.includes("login_throttle");
    const throttle=isLogin?loginThrottle:joinThrottle;
    if (/^SELECT failures,\s*locked_until FROM (login|join)_throttle WHERE id=\?$/.test(query)) {
      const row=throttle.get(args[0]);
      return {toArray:()=>row?[row]:[]};
    }
    if (query.startsWith("SELECT locked_until")) {
      const row=throttle.get(args[0]);
      return {toArray:()=>row?[row]:[]};
    }
    if (query.startsWith("INSERT INTO login_throttle") || query.startsWith("INSERT INTO join_throttle")) {
      throttle.set(args[0],{failures:args[1],locked_until:args[2]});
      return {toArray:()=>[]};
    }
    if (query.startsWith("DELETE FROM login_throttle") || query.startsWith("DELETE FROM join_throttle")) {
      throttle.delete(args[0]);return {toArray:()=>[]};
    }
    if (query.startsWith("SELECT id, public_key, salt, hash FROM devices")) {
      const row=records.get(args[0]);return {toArray:()=>row?[row]:[]};
    }
    if (query.startsWith("INSERT INTO devices")) {
      records.set(args[0],{id:args[0],public_key:args[1],salt:args[2],hash:args[3]});
      return {toArray:()=>[]};
    }
    throw new Error("Unexpected query: "+query);
  }};
  const hub=Object.create(SignalHub.prototype);
  hub.ctx={getWebSockets:()=>sockets,storage:{sql}};
  hub.env={};
  return {hub,records,loginThrottle,joinThrottle,sockets};
}
async function keyPair() {
  const pair=await crypto.subtle.generateKey({name:"Ed25519"},true,["sign","verify"]);
  const publicKey=hex(await crypto.subtle.exportKey("raw",pair.publicKey));
  return {pair,publicKey};
}
async function challenge(hub,socket,id,publicKey) {
  await hub.login(socket,socket.state,{type:"login",user_id:id,
    device_key:{version:1,public_key:publicKey}});
  const message=socket.messages.at(-1);
  assert.equal(message.type,"login_challenge");
  return message;
}
async function prove(hub,socket,id,key,challenge,password="") {
  const transcript=JSON.stringify(["crossdesk.device-login.v1",id,key.publicKey,challenge.nonce]);
  const signature=hex(await crypto.subtle.sign("Ed25519",key.pair.privateKey,encode.encode(transcript)));
  await hub.login(socket,socket.state,{type:"login",user_id:password?id+"@"+password:id,
    device_key:{version:1,public_key:key.publicKey,nonce:challenge.nonce,signature}});
}
test("CrossDesk signed login ignores stale device password and old device-ID lock",async()=>{
  const f=fixture(), key=await keyPair(), id="123456789";
  const salt="aa";
  const hash=hex(await crypto.subtle.digest("SHA-256",encode.encode(salt+":old123")));
  f.records.set(id,{id,public_key:key.publicKey,salt,hash});
  // A lock from the older Worker must not block the new signed login.
  f.loginThrottle.set(id,{failures:6,locked_until:seconds()+300});
  const primary=new Socket();f.sockets.push(primary);
  const nonce=await challenge(f.hub,primary,id,key.publicKey);
  assert.equal(nonce.password_required,false);
  await prove(f.hub,primary,id,key,nonce);
  assert.equal(primary.messages.at(-1).status,"success");
  assert.equal(primary.messages.at(-1).user_id,id);
  assert.equal(f.loginThrottle.get(id).failures,6);
});
test("signed challenge is consumed and a replay cannot log in",async()=>{
  const f=fixture(), key=await keyPair(), id="123456789";
  f.records.set(id,{id,public_key:key.publicKey,salt:"aa",hash:"00"});
  const primary=new Socket();f.sockets.push(primary);
  const nonce=await challenge(f.hub,primary,id,key.publicKey);
  const transcript=JSON.stringify(["crossdesk.device-login.v1",id,key.publicKey,nonce.nonce]);
  const signature=hex(await crypto.subtle.sign("Ed25519",key.pair.privateKey,encode.encode(transcript)));
  const response={type:"login",user_id:id,device_key:{version:1,public_key:key.publicKey,nonce:nonce.nonce,signature}};
  await f.hub.login(primary,primary.state,response);
  assert.equal(primary.messages.at(-1).status,"success");
  await f.hub.login(primary,primary.state,response);
  assert.equal(primary.messages.at(-1).reason,"already_logged_in");
});
test("join failures accumulate, lock and reset after expiration",()=>{
  const f=fixture(), id="123456789";
  for(let count=1;count<=6;count++) {
    f.hub.recordJoinFailure(id);
    assert.equal(f.joinThrottle.get(id).failures,count);
  }
  assert.equal(f.hub.joinThrottled(id),true);
  f.joinThrottle.set(id,{failures:6,locked_until:seconds()-1});
  assert.equal(f.hub.joinThrottled(id),false);
  f.hub.recordJoinFailure(id);
  assert.equal(f.joinThrottle.get(id).failures,1);
});
test("diagnostic classification never sends credential details to a client",async()=>{
  const f=fixture(), key=await keyPair(), other=await keyPair();
  const id="123456789", salt="bb";
  const hash=hex(await crypto.subtle.digest("SHA-256",encode.encode(salt+":654321")));
  f.records.set(id,{id,public_key:key.publicKey,salt,hash});
  const warnings=[];
  const oldWarn=console.warn;
  try {
    console.warn=(...args)=>warnings.push(args);
    f.hub.env.DEBUG_LOGIN="1";
    const wrongKey=new Socket();f.sockets.push(wrongKey);
    const nonce=await challenge(f.hub,wrongKey,id,other.publicKey);
    await prove(f.hub,wrongKey,id,other,nonce);
    assert.equal(warnings.at(-1)[1].check,"device_key_mismatch");
    assert.equal(wrongKey.messages.at(-1).reason,"invalid_credentials");
    assert.equal(wrongKey.messages.at(-1).check,undefined);
    const unexpectedPassword=new Socket();f.sockets.push(unexpectedPassword);
    const nonce2=await challenge(f.hub,unexpectedPassword,id,key.publicKey);
    await prove(f.hub,unexpectedPassword,id,key,nonce2,"111111");
    assert.equal(warnings.at(-1)[1].check,"unexpected_password");
    assert.equal(unexpectedPassword.messages.at(-1).check,undefined);
    assert.ok(!JSON.stringify(warnings).includes(key.publicKey));
    assert.ok(!JSON.stringify(warnings).includes("111111"));
  } finally {console.warn=oldWarn;}
});
