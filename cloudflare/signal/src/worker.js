/*
 * ReDesk Cloudflare Worker signaling prototype.
 *
 * Durable Object owns authentication, presence and authorized peer routing.
 * NO media or relay bytes are carried through Worker WebSockets.
 * STUN-only ICE is deliberately the first milestone; TURN requires a separate
 * service and per-connection credentials. Do not expose privileged endpoints.
 */

const MAX_MESSAGE = 64 * 1024;
const CHALLENGE_MS = 30_000;
const MAX_CONNECTIONS = 200; // Single hub is for initial testing, not scaling.
const ICE_LIFETIME = 1800;
const ID = /^\d{9}$/;
// MiniRTC creates a second signaling connection for each outgoing session,
// using C-<own ID>. It must prove possession of the registered device key.
const SESSION_ID = /^C-\d{9}$/;
const HEX32 = /^[0-9a-f]{64}$/;
const HEX64 = /^[0-9a-f]{128}$/;
const ROOM_FIELDS = new Set(["offer", "answer", "new_candidate", "new_candidate_mid"]);

function hex(bytes) {
  return [...bytes].map(x => x.toString(16).padStart(2, "0")).join("");
}
function unhex(s) {
  return Uint8Array.from(s.match(/../g), x => parseInt(x, 16));
}
function randomHex(n = 32) {
  return hex(crypto.getRandomValues(new Uint8Array(n)));
}
function randomDigits(n) {
  const bytes = crypto.getRandomValues(new Uint8Array(n));
  return [...bytes].map(n => n % 10).join("");
}
function now() { return Math.floor(Date.now() / 1000); }
function validName(s) { return typeof s === "string" && ID.test(s); }
function validPeerName(s) { return typeof s === "string" && (ID.test(s) || SESSION_ID.test(s)); }
function ownerId(s) { return SESSION_ID.test(s) ? s.slice(2) : s; }
function safeSend(socket, obj) {
  try { socket.send(JSON.stringify(obj)); return true; } catch { return false; }
}
function ctEqual(a, b) {
  if (typeof a !== "string" || typeof b !== "string" || a.length !== b.length) return false;
  let diff = 0;
  for (let i = 0; i < a.length; i++) diff |= a.charCodeAt(i) ^ b.charCodeAt(i);
  return diff === 0;
}
async function passwordDigest(password, salt) {
  return hex(new Uint8Array(await crypto.subtle.digest("SHA-256",
    new TextEncoder().encode(salt + ":" + password))));
}
async function verifyProof(keyHex, id, nonce, signatureHex) {
  if (!HEX32.test(keyHex) || !HEX32.test(nonce) || !HEX64.test(signatureHex)) return false;
  try {
    const key = await crypto.subtle.importKey("raw", unhex(keyHex), {name:"Ed25519"}, false, ["verify"]);
    const transcript = JSON.stringify(["crossdesk.device-login.v1", id, keyHex, nonce]);
    return await crypto.subtle.verify("Ed25519", key, unhex(signatureHex),
      new TextEncoder().encode(transcript));
  } catch { return false; }
}
function iceSnapshot() {
  return {
    version: 1, id: crypto.randomUUID(), expires_at: now() + ICE_LIFETIME,
    servers: [{urls:["stun:stun.cloudflare.com:3478"]}]
  };
}
function reject(socket, reason, type = "error") { safeSend(socket, {type, status:"failed", reason}); }
function identifier(raw) { return raw.slice(0, raw.indexOf("@") < 0 ? undefined : raw.indexOf("@")); }

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    if (url.pathname === "/health" && request.method === "GET") {
      return Response.json({ok:true, service:"ReDesk signaling", protocol:"crossdesk-minirtc-v1", media_relay:false});
    }
    if (url.pathname !== "/signal" || request.headers.get("Upgrade")?.toLowerCase() !== "websocket") {
      return new Response("Not found", {status:404});
    }
    // Named DO ensures WS clients and their account directory share one
    // coordinator. Split/shard only when cross-shard routing is implemented.
    const obj = env.SIGNAL_HUB.get(env.SIGNAL_HUB.idFromName("redesk-v1"));
    return obj.fetch(request);
  }
};

export class SignalHub {
  constructor(ctx, env) {
    this.ctx = ctx;
    this.env = env;
    this.ctx.storage.sql.exec(
      "CREATE TABLE IF NOT EXISTS devices (id TEXT PRIMARY KEY, public_key TEXT NOT NULL, salt TEXT NOT NULL, hash TEXT NOT NULL)");
    this.ctx.storage.sql.exec(
      "CREATE TABLE IF NOT EXISTS login_throttle (id TEXT PRIMARY KEY, failures INTEGER NOT NULL, locked_until INTEGER NOT NULL)");
    this.ctx.storage.sql.exec(
      "CREATE TABLE IF NOT EXISTS join_throttle (id TEXT PRIMARY KEY, failures INTEGER NOT NULL, locked_until INTEGER NOT NULL)");
    this.ctx.storage.sql.exec(
      "CREATE TABLE IF NOT EXISTS sessions (token TEXT PRIMARY KEY, host TEXT NOT NULL, guest TEXT NOT NULL, created INTEGER NOT NULL)");
    // Session authorization must survive DO hibernation. SQLite persists
    // tickets; socket.serializeAttachment persists authenticated identity.
  }

  async fetch(request) {
    if (this.ctx.getWebSockets().length >= MAX_CONNECTIONS)
      return new Response("Capacity reached", {status:503});
    const pair = new WebSocketPair();
    this.ctx.acceptWebSocket(pair[1]);
    pair[1].serializeAttachment({phase:"new", at:Date.now(), user:null});
    return new Response(null, {status:101, webSocket:pair[0]});
  }

  socket(id) {
    if (!validPeerName(id)) return null;
    // getWebSockets() returns sockets still associated with this Durable Object,
    // including those restored after WebSocket hibernation. Some runtime/mock
    // adapters don't expose readyState consistently on these server-side
    // handles. The durable authenticated attachment, not a numeric state
    // comparison, is the identity source of truth.
    return this.ctx.getWebSockets().find(ws => {
      const state = ws.deserializeAttachment();
      return state?.phase === "authenticated" && state.user === id;
    }) ?? null;
  }
  online(id) { return validName(id) && this.socket(id) !== null; }
  send(id, data) {
    const target = this.socket(id);
    return target ? safeSend(target, data) : false;
  }
  updatePresence(id, online) {
    for (const ws of this.ctx.getWebSockets()) {
      const att = ws.deserializeAttachment();
      if (att?.phase === "authenticated" && att.subscriptions?.includes(id)) {
        safeSend(ws, {type:"presence_update", id, online});
      }
    }
  }
  failure(id) {
    if (!id) return;
    const old = this.ctx.storage.sql.exec("SELECT failures, locked_until FROM login_throttle WHERE id=?", id).toArray()[0];
    const count = (old?.locked_until > now() ? old.failures : 0) + 1;
    const locked = count >= 6 ? now() + 300 : 0;
    this.ctx.storage.sql.exec(
      "INSERT INTO login_throttle(id,failures,locked_until) VALUES(?,?,?) ON CONFLICT(id) DO UPDATE SET failures=excluded.failures,locked_until=excluded.locked_until",
      id, count, locked);
  }
  throttled(id) {
    const row = this.ctx.storage.sql.exec("SELECT locked_until FROM login_throttle WHERE id=?", id).toArray()[0];
    return row && row.locked_until > now();
  }
  clearFailure(id) {
    this.ctx.storage.sql.exec("DELETE FROM login_throttle WHERE id=?", id);
  }
  // Diagnostics report only a peer category and a protocol reason; never log
  // device identifiers, credentials, Ed25519 keys, SDP or ICE candidates.
  authLog(event, id, reason = "") {
    if (event !== "rejected" && this.env?.DEBUG_LOGIN !== "1") return;
    const kind = !id ? "new" : SESSION_ID.test(id) ? "viewer" :
      validName(id) ? "device" : "invalid";
    const detail = {event, kind, reason};
    if (event === "rejected") console.warn("redesk.auth", detail);
    else console.info("redesk.auth", detail);
  }
  denyLogin(socket, id, reason) {
    this.authLog("rejected", id, reason);
    reject(socket, reason, "login");
  }
  account(id) {
    return this.ctx.storage.sql.exec("SELECT id, public_key, salt, hash FROM devices WHERE id=?", id).toArray()[0];
  }
  joinThrottled(id) {
    const row=this.ctx.storage.sql.exec("SELECT locked_until FROM join_throttle WHERE id=?",id).toArray()[0];
    return Boolean(row && row.locked_until > now());
  }
  recordJoinFailure(id) {
    const row=this.ctx.storage.sql.exec("SELECT failures,locked_until FROM join_throttle WHERE id=?",id).toArray()[0];
    const count=(row?.locked_until>now()?row.failures:0)+1;
    const until=count>=6?now()+300:0;
    this.ctx.storage.sql.exec(
      "INSERT INTO join_throttle(id,failures,locked_until) VALUES(?,?,?) ON CONFLICT(id) DO UPDATE SET failures=excluded.failures,locked_until=excluded.locked_until",
      id,count,until);
  }

  async webSocketMessage(socket, data) {
    if (typeof data !== "string" || data.length > MAX_MESSAGE) { socket.close(1009, "Too large"); return; }
    let msg;
    try { msg = JSON.parse(data); } catch { reject(socket,"invalid_json"); return; }
    if (!msg || typeof msg !== "object" || Array.isArray(msg) || typeof msg.type !== "string") {
      reject(socket,"invalid_message"); return;
    }
    const state = socket.deserializeAttachment() ?? {phase:"new"};
    // Limit traffic from unauthenticated clients.
    if (state.phase !== "authenticated" && Date.now() - (state.at ?? 0) > CHALLENGE_MS) {
      socket.close(1008, "Login timeout"); return;
    }
    if (msg.type === "login") {
      await this.login(socket, state, msg);
      return;
    }
    if (state.phase !== "authenticated") { reject(socket,"not_authenticated"); return; }
    const user = state.user;
    if (msg.type === "recent_connections_presence") {
      const devices = Array.isArray(msg.devices) ? msg.devices.filter(validName).slice(0, 128) : [];
      // A one-shot probe must not erase long-lived presence subscriptions.
      if (msg.subscribe !== false) {
        state.subscriptions = devices;
        socket.serializeAttachment(state);
      }
      const results = devices.map(id => ({id, online:this.online(id)}));
      if (this.env?.DEBUG_PRESENCE === "1") {
        // Never log IDs, passwords, device public keys or SDP.
        console.info("redesk.presence", {
          requested: results.length,
          online: results.filter(x => x.online).length,
          sockets: this.ctx.getWebSockets().length,
          one_shot: msg.subscribe === false
        });
      }
      safeSend(socket, {type:"presence", devices:results});
    } else if (msg.type === "join_transmission") {
      await this.join(socket, user, msg);
    } else if (msg.type === "user_leave_transmission") {
      this.leave(user, msg.transmission_id);
    } else if (msg.type === "disconnect_peer") {
      this.leave(user, msg.transmission_id);
    } else if (ROOM_FIELDS.has(msg.type)) {
      this.relay(socket, user, msg);
    } else if (msg.type === "session_report") {
      // No authorization is derived from client session reports.
    } else if (msg.type === "client_info" || msg.type === "ping") {
      // Optional telemetry, intentionally ignored (no personal data stored).
    } else {
      reject(socket,"unsupported_type",msg.type);
    }
  }

  async login(socket, state, msg) {
    if (state.phase === "authenticated") { reject(socket,"already_logged_in"); return; }
    if (state.phase === "new") {
      if (typeof msg.user_id !== "string" || msg.user_id.length > 32 ||
          !msg.device_key || msg.device_key.version !== 1 ||
          !HEX32.test(msg.device_key.public_key)) {
        this.denyLogin(socket, "", "invalid_login"); return;
      }
      const id = identifier(msg.user_id);
      if (id !== "" && !validPeerName(id)) { this.denyLogin(socket,id,"invalid_id"); return; }
      const parent = ownerId(id);
      // Viewer sessions use a separate login-throttle namespace. A failing
      // temporary session must not lock the registered host's primary login.
      if (id && this.throttled(id)) { this.denyLogin(socket,id,"rate_limited"); return; }
      const account = parent ? this.account(parent) : null;
      if (parent && !account) { this.denyLogin(socket,id,"unknown_id"); return; }
      // C- sessions authenticate with their owner's Ed25519 key, not a
      // reusable password. Never create a new device record for C- IDs.
      const passwordRequired = Boolean(account) && !SESSION_ID.test(id);
      const nonce = randomHex();
      socket.serializeAttachment({phase:"challenge",at:Date.now(),user:null,id,
        key:msg.device_key.public_key,nonce, passwordRequired});
      safeSend(socket, {type:"login_challenge",user_id:id,nonce,device_key_version:1,
        password_required:passwordRequired});
      this.authLog("challenge", id);
      return;
    }
    if (state.phase !== "challenge" || Date.now() - state.at > CHALLENGE_MS) {
      this.denyLogin(socket,state.id ?? "","invalid_login_state"); return;
    }
    const id = state.id;
    const presented = typeof msg.user_id === "string" ? msg.user_id : "";
    const password = presented.includes("@") ? presented.slice(presented.indexOf("@") + 1) : "";
    if (identifier(presented) !== id || !msg.device_key ||
        msg.device_key.public_key !== state.key || msg.device_key.nonce !== state.nonce ||
        !(await verifyProof(state.key, id, state.nonce, msg.device_key.signature))) {
      this.failure(id); this.denyLogin(socket,id,"invalid_proof");
      socket.close(1008,"Authentication failed"); return;
    }
    let assigned = id;
    let initialPassword = null;
    if (SESSION_ID.test(id)) {
      // Only the physical device that owns the numeric ID may use C-<ID>.
      // The signed challenge binds this temporary session to that device key.
      const parent = ownerId(id);
      const row = this.account(parent);
      if (!row || row.public_key !== state.key || password || presented.includes("@")) {
        this.failure(id); this.denyLogin(socket,id,"invalid_credentials");
        socket.close(1008,"Authentication failed"); return;
      }
      this.clearFailure(id);
    } else if (id) {
      const row = this.account(id);
      if (!row || row.public_key !== state.key ||
          !ctEqual(row.hash, await passwordDigest(password, row.salt))) {
        this.failure(id); this.denyLogin(socket,id,"invalid_credentials");
        socket.close(1008,"Authentication failed"); return;
      }
      this.clearFailure(id);
    } else {
      // A new ID is allocated only after proof of the client's private key.
      for (let i=0; i<25; i++) {
        const candidate = String(100000000 + Math.floor(crypto.getRandomValues(new Uint32Array(1))[0] / 4294967296 * 900000000));
        if (!this.account(candidate)) { assigned = candidate; break; }
      }
      if (!assigned) { this.denyLogin(socket,id,"capacity"); return; }
      initialPassword = randomDigits(6);
      const salt = randomHex(16);
      const hash = await passwordDigest(initialPassword, salt);
      this.ctx.storage.sql.exec("INSERT INTO devices(id,public_key,salt,hash) VALUES(?,?,?,?)",
        assigned,state.key,salt,hash);
    }

    // Close an older connection to keep one authoritative peer per ID.
    const prior = this.socket(assigned);
    if (prior && prior !== socket) {
      this.authLog("replaced_existing_socket", assigned);
      prior.close(4001,"New login");
    }
    socket.serializeAttachment({phase:"authenticated",at:Date.now(),user:assigned,subscriptions:[]});
    safeSend(socket,{type:"login",status:"success",
      user_id:initialPassword ? assigned+"@"+initialPassword : assigned,
      device_key_version:1,ice_config_version:1});
    // Viewer C- sessions are ephemeral and must not alter the owner's
    // presence, which is based on the primary numeric-ID signaling socket.
    if (validName(assigned)) this.updatePresence(assigned,true);
    this.authLog("success", assigned);
  }

  async join(socket, user, msg) {
    if (typeof msg.transmission_id !== "string" || msg.transmission_id.length > 40) {
      reject(socket,"invalid_target","user_join_transmission"); return;
    }
    const target = identifier(msg.transmission_id);
    const password = msg.transmission_id.includes("@") ? msg.transmission_id.split("@",2)[1] : "";
    const roomFail = reason => safeSend(socket,{type:"user_join_transmission",status:"failed",
      transmission_id:msg.transmission_id,reason});
    if (!validName(target) || target === user) { roomFail("invalid_target"); return; }
    const account = this.account(target);
    if (!account || !this.online(target)) { roomFail("remote_unavailable"); return; }
    if (this.joinThrottled(target)) { roomFail("rate_limited"); return; }
    if (!password || !ctEqual(account.hash,await passwordDigest(password,account.salt))) {
      this.recordJoinFailure(target);
      roomFail("incorrect_password"); return;
    }
    this.ctx.storage.sql.exec("DELETE FROM join_throttle WHERE id=?",target);
    const ticket = randomHex();
    this.ctx.storage.sql.exec(
      "INSERT INTO sessions(token,host,guest,created) VALUES(?,?,?,?)",
      ticket,target,user,now());
    const ice=iceSnapshot();
    // Host and viewer receive the same canonical transmission ID (without its
    // secret), plus the *other* endpoint's authenticated identity.
    const sessionTicket = {type:"session_ticket",transmission_id:target,
      host_id:target,guest_id:user,token:ticket};
    // The host (offerer) alone receives user_join_transmission and creates
    // the SDP offer. Sending user_join_transmission to the viewer too makes
    // both peers offer simultaneously and resets ICE transports.
    if (!this.send(target,sessionTicket)) {
      this.ctx.storage.sql.exec("DELETE FROM sessions WHERE token=?",ticket);
      roomFail("remote_unavailable");
      return;
    }
    safeSend(socket,sessionTicket);
    if (!this.send(target,{type:"user_join_transmission",status:"success",
      transmission_id:target,user_id:user,ice})) {
      this.ctx.storage.sql.exec("DELETE FROM sessions WHERE token=?",ticket);
      roomFail("remote_unavailable");
    }
  }
  room(user,target,transmission) {
    this.ctx.storage.sql.exec("DELETE FROM sessions WHERE created<?", now()-7200);
    return this.ctx.storage.sql.exec(
      "SELECT token FROM sessions WHERE host=? AND ((host=? AND guest=?) OR (host=? AND guest=?)) LIMIT 1",
      transmission,user,target,target,user).toArray()[0] ?? null;
  }
  relay(socket,user,msg) {
    const target=msg.remote_user_id,transmission=identifier(msg.transmission_id ?? "");
    if (!validPeerName(target) || !validName(transmission) ||
        !this.room(user,target,transmission)) {reject(socket,"not_in_session");return;}
    const payload=JSON.stringify(msg);
    if (payload.length>MAX_MESSAGE ||
        ((msg.type==="offer" || msg.type==="answer") && (typeof msg.sdp!=="string" || msg.sdp.length>48*1024)) ||
        ((msg.type==="new_candidate" || msg.type==="new_candidate_mid") &&
          !["sdp","candidate"].some(k=>typeof msg[k]==="string" && msg[k].length<4096))) {
      reject(socket,"invalid_signal");return;
    }
    // Never trust sender-supplied user_id; recipients should see the
    // authenticated sender as their remote peer.
    this.send(target,{...msg,user_id:user,remote_user_id:user,transmission_id:transmission});
  }
  leave(user,rawTarget) {
    const target=identifier(typeof rawTarget==="string" ? rawTarget : "");
    const rows=this.ctx.storage.sql.exec(
      "SELECT token,host,guest FROM sessions WHERE (host=? OR guest=?)",user,user).toArray();
    for (const r of rows) {
      if (target && r.host!==target) continue;
      const other=r.host===user?r.guest:r.host;
      this.send(other,{type:"user_leave_transmission",user_id:user,transmission_id:r.host});
      this.ctx.storage.sql.exec("DELETE FROM sessions WHERE token=?",r.token);
    }
  }
  webSocketClose(socket) { this.cleanup(socket); }
  webSocketError(socket) { this.cleanup(socket); }
  cleanup(socket) {
    const state=socket.deserializeAttachment();
    if (state?.phase!=="authenticated") return;
    const id=state.user;
    const rows=this.ctx.storage.sql.exec(
      "SELECT token,host,guest FROM sessions WHERE host=? OR guest=?",id,id).toArray();
    for (const r of rows) {
      const other=r.host===id?r.guest:r.host;
      this.send(other,{type:"user_leave_transmission",user_id:id,transmission_id:r.host});
      this.ctx.storage.sql.exec("DELETE FROM sessions WHERE token=?",r.token);
    }
    if (validName(id) && !this.online(id)) this.updatePresence(id,false);
  }
}
