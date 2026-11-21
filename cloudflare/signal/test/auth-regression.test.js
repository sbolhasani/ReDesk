import test from "node:test";
import assert from "node:assert/strict";
import { SignalHub } from "../src/worker.js";

class FakeSocket {
  constructor() { this.state={phase:"new",at:Date.now(),user:null};this.messages=[]; }
  deserializeAttachment() { return structuredClone(this.state); }
  serializeAttachment(state) { this.state=structuredClone(state); }
  send(text) { this.messages.push(JSON.parse(text)); }
  close() { this.closed=true; }
}
function setup() {
  const sockets=[];
  const failures=new Map();
  const instance=Object.create(SignalHub.prototype);
  instance.ctx={getWebSockets:()=>sockets};
  instance.env={};
  instance.account=id=>(["123456789","987654321"].includes(id) ?
    {id,public_key:"a".repeat(64),salt:"test",hash:"test"}:null);
  instance.throttled=id=>(failures.get(id)??0)>=6;
  instance.failure=id=>failures.set(id,(failures.get(id)??0)+1);
  instance.clearFailure=id=>failures.delete(id);
  return {instance,sockets,failures};
}
test("both registered primary IDs receive independent login challenges",async()=>{
  const {instance,sockets}=setup();
  const first=new FakeSocket(),second=new FakeSocket();
  sockets.push(first,second);
  for(const [socket,id] of [[first,"123456789"],[second,"987654321"]]) {
    await instance.login(socket,socket.state,{
      type:"login",user_id:id,device_key:{version:1,public_key:"a".repeat(64)}
    });
    assert.equal(socket.messages.at(-1).type,"login_challenge");
    assert.equal(socket.messages.at(-1).user_id,id);
    assert.equal(socket.messages.at(-1).password_required,false);
  }
});
test("existing legacy login lock does not block signed primary login challenge",async()=>{
  const {instance,sockets,failures}=setup();
  const viewer=new FakeSocket();
  sockets.push(viewer);
  for(let n=0;n<6;n++) {
    instance.failure("C-123456789");
  }
  assert.equal(instance.throttled("C-123456789"),true);
  for(let n=0;n<6;n++) instance.failure("123456789");
  assert.equal(instance.throttled("123456789"),true);
  assert.equal(instance.throttled("123456789"),true);
  const host=new FakeSocket();sockets.push(host);
  await instance.login(host,host.state,{
    type:"login",user_id:"123456789",device_key:{version:1,public_key:"a".repeat(64)}
  });
  assert.equal(host.messages.at(-1).type,"login_challenge");
  assert.equal(failures.get("123456789"),6);
});
test("temporary viewer login challenge requires registered owner but not password",async()=>{
  const {instance,sockets}=setup();
  const viewer=new FakeSocket();sockets.push(viewer);
  await instance.login(viewer,viewer.state,{
    type:"login",user_id:"C-987654321",device_key:{version:1,public_key:"a".repeat(64)}
  });
  assert.equal(viewer.messages.at(-1).type,"login_challenge");
  assert.equal(viewer.messages.at(-1).user_id,"C-987654321");
  assert.equal(viewer.messages.at(-1).password_required,false);
});
