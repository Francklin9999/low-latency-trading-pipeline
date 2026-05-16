const fs = require("fs");
const http = require("http");
const net = require("net");
const path = require("path");
const { URL } = require("url");

const amqp = require("amqplib");
const { Pool } = require("pg");

const PORT = Number(process.env.PORT || 8080);
const TELEMETRY_SOCKET = process.env.TELEMETRY_SOCKET
  || path.join(process.cwd(), "run", "hft-telemetry.sock");
const CONTROL_SOCKET = process.env.CONTROL_SOCKET
  || path.join(process.cwd(), "run", "hft-control.sock");
const AMQP_URL = process.env.AMQP_URL || "amqp://rabbitmq:5672";
const PG_URL = process.env.DATABASE_URL || "postgres://hft:hft@postgres:5432/hft";
const DASHBOARD_DIR = process.env.DASHBOARD_DIR || path.join(__dirname, "dashboard");
const DASHBOARD_LOG_DIR = process.env.HFT_DASHBOARD_LOG_DIR
  || path.join(process.cwd(), "results", "dashboard_logs");
const MAX_PENDING_ORDERS = 100000;
const PENDING_ORDER_TTL_NS = 5 * 60 * 1e9;

const SYMBOLS = {
  13: "AAPL",
  347: "AMD",
  2020: "DIA",
  2679: "EWZ",
  2741: "FB",
  3461: "GOOGL",
  4190: "INTC",
  4321: "IWM",
  5294: "MSFT",
  5336: "MU",
  6562: "QQQ",
  7159: "SH",
  7457: "SPY",
  7468: "SQQQ",
  7888: "TNA",
  7939: "TQQQ",
  8000: "TSLA",
  8036: "TVIX",
  8757: "XLK"
};

const state = {
  requestedGlobalKill: false,
  latestHealth: null,
  recentOrders: [],
  recentAcks: [],
  symbols: {},
  feed: {
    lastSeq: 0,
    packets: 0,
    messages: 0
  },
  // Pushed every 2s by trading's telemetry thread.
  latency: null,        // { p50, p90, p99, max, n, label, ts }
  mmStats: {}           // locate -> { quotes, fills, inventory, rPnL, uPnL, skip_*, rate_limited, ts }
};

const clients = new Set();
const pg = new Pool({ connectionString: PG_URL });
const pendingOrders = new Map();

let amqpConn = null;
let amqpChannel = null;
let auditStream = null;
let auditDayKey = "";
let auditPath = "";

function symbolName(locate) {
  return SYMBOLS[locate] || `LOC-${locate}`;
}

function currentIso() {
  return new Date().toISOString();
}

function dayKeyFor(date = new Date()) {
  const year = date.getFullYear();
  const month = String(date.getMonth() + 1).padStart(2, "0");
  const day = String(date.getDate()).padStart(2, "0");
  return `${year}${month}${day}`;
}

function ensureAuditStream() {
  const dayKey = dayKeyFor();
  if (auditStream && auditDayKey === dayKey) return;

  if (auditStream) {
    auditStream.end();
    auditStream = null;
  }

  fs.mkdirSync(DASHBOARD_LOG_DIR, { recursive: true });
  auditDayKey = dayKey;
  auditPath = path.join(DASHBOARD_LOG_DIR, `dashboard_${dayKey}.txt`);
  auditStream = fs.createWriteStream(auditPath, { flags: "a" });
  auditStream.on("error", (err) => {
    console.error("[control] audit log stream failed:", err.message);
  });
  console.log(`[control] dashboard log -> ${auditPath}`);
}

function writeAuditRecord(kind, payload) {
  try {
    ensureAuditStream();
    if (!auditStream) return;
    auditStream.write(`${JSON.stringify({
      kind,
      loggedAt: currentIso(),
      ...payload
    })}\n`);
  } catch (err) {
    console.error("[control] audit log write failed:", err.message);
  }
}

function nsDelta(later, earlier) {
  if (!Number.isFinite(later) || !Number.isFinite(earlier) || later < earlier) {
    return null;
  }
  return later - earlier;
}

function prunePendingOrders(nowTs) {
  if (pendingOrders.size === 0) return;
  const cutoff = Number.isFinite(nowTs) ? nowTs - PENDING_ORDER_TTL_NS : null;
  for (const [seq, order] of pendingOrders) {
    if (cutoff !== null && Number.isFinite(order.ts) && order.ts < cutoff) {
      pendingOrders.delete(seq);
      continue;
    }
    if (pendingOrders.size <= MAX_PENDING_ORDERS) break;
    pendingOrders.delete(seq);
  }
}

function rememberOrder(event) {
  pendingOrders.set(event.seq, {
    seq: event.seq,
    symbolId: event.symbolId,
    symbol: symbolName(event.symbolId),
    side: event.side,
    qty: event.qty,
    price: event.price,
    msgType: event.msgType,
    ts: event.ts,
    clientSendTs: event.clientSendTs
  });
  prunePendingOrders(event.ts);
}

function emitOrderAckAudit(ack) {
  const order = pendingOrders.get(ack.seq);
  if (!order) return;
  pendingOrders.delete(ack.seq);
  writeAuditRecord("order_ack", {
    seq: ack.seq,
    symbolId: order.symbolId,
    symbol: order.symbol,
    side: order.side,
    qty: order.qty,
    price: order.price,
    msgType: order.msgType,
    orderObserveTs: order.ts,
    clientSendTs: order.clientSendTs,
    ackObserveTs: ack.ts,
    serverRecvTs: ack.recvTs,
    ackFlag: ack.flag,
    observerRoundTripNs: nsDelta(ack.ts, order.ts),
    clientSendToAckObserveNs: nsDelta(ack.ts, order.clientSendTs),
    clientSendToServerRecvNs: nsDelta(ack.recvTs, order.clientSendTs)
  });
}

async function ensureSchema() {
  await pg.query(`
    create table if not exists feed_packets (
      ts bigint not null,
      seq bigint not null,
      msg_count integer not null,
      payload_bytes integer not null
    );
    create table if not exists wire_orders (
      ts bigint not null,
      seq bigint not null,
      symbol_id integer not null,
      side char(1) not null,
      qty integer not null,
      price integer not null,
      msg_type char(1) not null
    );
    create table if not exists wire_acks (
      ts bigint not null,
      seq bigint not null,
      flag integer not null,
      recv_ts bigint not null
    );
    create table if not exists observer_health (
      ts bigint not null,
      feed_packets bigint not null,
      feed_messages bigint not null,
      orders bigint not null,
      acks bigint not null,
      ack_ok bigint not null,
      ack_bad_csum bigint not null,
      ack_seq_gap bigint not null,
      last_ack_seq bigint not null,
      transport_drops bigint not null
    );
  `);
}

async function connectRabbit() {
  try {
    amqpConn = await amqp.connect(AMQP_URL);
    amqpConn.on("close", () => {
      amqpConn = null;
      amqpChannel = null;
      setTimeout(connectRabbit, 1000);
    });
    amqpConn.on("error", () => {});

    amqpChannel = await amqpConn.createChannel();
    await amqpChannel.assertExchange("hft.telemetry", "topic", { durable: true });
    await amqpChannel.assertExchange("hft.control", "topic", { durable: true });
  } catch (err) {
    console.error("[control] rabbitmq unavailable:", err.message);
    setTimeout(connectRabbit, 1000);
  }
}

function publishAmqp(exchange, routingKey, payload) {
  if (!amqpChannel) return;
  amqpChannel.publish(exchange, routingKey, Buffer.from(JSON.stringify(payload)), {
    contentType: "application/json",
    persistent: true
  });
}

function snapshotState() {
  const symbols = Object.values(state.symbols).sort((a, b) => a.locate - b.locate);
  const totals = symbols.reduce((acc, symbol) => {
    acc.activeQuotes += (symbol.ourBidPx ? 1 : 0) + (symbol.ourAskPx ? 1 : 0);
    acc.orders += symbol.orders || 0;
    const mm = state.mmStats[symbol.locate];
    if (mm) {
      acc.realizedPnlTicks   += Number(mm.realized_pnl_ticks   || 0);
      acc.unrealizedPnlTicks += Number(mm.unrealized_pnl_ticks || 0);
      acc.fills              += Number(mm.fills                || 0);
    }
    return acc;
  }, { activeQuotes: 0, orders: 0, realizedPnlTicks: 0, unrealizedPnlTicks: 0, fills: 0 });

  // Splice per-symbol mm_stats (inventory + PnL) onto the symbols array so
  // the dashboard can render them in one table without a second loop.
  const enriched = symbols.map((sym) => {
    const mm = state.mmStats[sym.locate];
    return mm ? { ...sym, mm } : sym;
  });

  return {
    requestedGlobalKill: state.requestedGlobalKill,
    latestHealth: state.latestHealth,
    recentOrders: state.recentOrders,
    recentAcks: state.recentAcks,
    feed: state.feed,
    symbols: enriched,
    totals,
    latency: state.latency
  };
}

// SSE backpressure: skip the write if a client's socket buffer climbs past this.
// The dashboard reconciles from the next snapshot, so dropping a frame is fine.
const SSE_BACKPRESSURE_BYTES = 1 << 20;   // 1 MiB

function broadcast(payload) {
  const line = `data: ${JSON.stringify(payload)}\n\n`;
  for (const client of clients) {
    const queued = (client.socket && client.socket.writableLength) ||
                   client.writableLength || 0;
    if (queued > SSE_BACKPRESSURE_BYTES) continue;   // stalled tab -- skip
    client.write(line);
  }
}

// Throttle full-snapshot broadcasts to ~10 Hz. Order/ack events arrive at
// thousands/sec; serialising snapshotState() that often was the OOM leak.
const SNAPSHOT_HZ = 10;
let lastSnapshotMs = 0;
let snapshotPending = false;
function maybeBroadcastSnapshot(event) {
  const now = Date.now();
  if (now - lastSnapshotMs >= 1000 / SNAPSHOT_HZ) {
    lastSnapshotMs = now;
    broadcast({ kind: "telemetry", event, snapshot: snapshotState() });
    snapshotPending = false;
  } else if (!snapshotPending) {
    snapshotPending = true;
    setTimeout(() => {
      if (!snapshotPending) return;
      lastSnapshotMs = Date.now();
      snapshotPending = false;
      broadcast({ kind: "telemetry", event: null, snapshot: snapshotState() });
    }, (1000 / SNAPSHOT_HZ) - (now - lastSnapshotMs));
  }
}

async function persistEvent(event) {
  switch (event.type) {
    case "feed":
      await pg.query(
        "insert into feed_packets (ts, seq, msg_count, payload_bytes) values ($1,$2,$3,$4)",
        [event.ts, event.seq, event.msgCount, event.payloadBytes]
      );
      break;
    case "order":
      await pg.query(
        "insert into wire_orders (ts, seq, symbol_id, side, qty, price, msg_type) values ($1,$2,$3,$4,$5,$6,$7)",
        [event.ts, event.seq, event.symbolId, event.side, event.qty, event.price, event.msgType]
      );
      break;
    case "ack":
      await pg.query(
        "insert into wire_acks (ts, seq, flag, recv_ts) values ($1,$2,$3,$4)",
        [event.ts, event.seq, event.flag, event.recvTs]
      );
      break;
    case "health":
      await pg.query(
        "insert into observer_health (ts, feed_packets, feed_messages, orders, acks, ack_ok, ack_bad_csum, ack_seq_gap, last_ack_seq, transport_drops) values ($1,$2,$3,$4,$5,$6,$7,$8,$9,$10)",
        [
          event.ts, event.feedPackets, event.feedMessages, event.orders, event.acks,
          event.ackOk, event.ackBadCsum, event.ackSeqGap, event.lastAckSeq, event.transportDrops
        ]
      );
      break;
  }
}

function upsertSymbol(locate) {
  if (!state.symbols[locate]) {
    state.symbols[locate] = {
      locate,
      symbol: symbolName(locate),
      ourBidPx: 0,
      ourAskPx: 0,
      orders: 0,
      lastOrderSeq: 0
    };
  }
  return state.symbols[locate];
}

async function handleTelemetryEvent(event) {
  if (!event || !event.type) return;

  writeAuditRecord("telemetry", { event });

  if (event.type === "feed") {
    state.feed.lastSeq = event.seq;
    state.feed.packets += 1;
    state.feed.messages += Number(event.msgCount || 0);
  } else if (event.type === "order") {
    const symbol = upsertSymbol(event.symbolId);
    symbol.orders += 1;
    symbol.lastOrderSeq = event.seq;
    if (event.msgType === "N") {
      if (event.side === "B") symbol.ourBidPx = event.price;
      else if (event.side === "S") symbol.ourAskPx = event.price;
    } else if (event.msgType === "C") {
      if (event.side === "B") symbol.ourBidPx = 0;
      else if (event.side === "S") symbol.ourAskPx = 0;
    } else if (event.msgType === "F") {
      symbol.ourBidPx = 0;
      symbol.ourAskPx = 0;
    }
    state.recentOrders.unshift({
      ...event,
      symbol: symbol.symbol
    });
    state.recentOrders = state.recentOrders.slice(0, 20);
    rememberOrder(event);
  } else if (event.type === "ack") {
    state.recentAcks.unshift(event);
    state.recentAcks = state.recentAcks.slice(0, 20);
    emitOrderAckAudit(event);
  } else if (event.type === "health") {
    state.latestHealth = event;
    state.feed.packets = Number(event.feedPackets || 0);
    state.feed.messages = Number(event.feedMessages || 0);
  } else if (event.type === "mm_stats") {
    // Per-symbol PnL + inventory + skip counters from trading's telemetry.
    for (const s of (event.symbols || [])) {
      state.mmStats[s.locate] = { ...s, ts: event.ts };
      // Make sure the per-symbol row exists even before the observer has
      // seen an outgoing order on this locate.
      upsertSymbol(s.locate);
    }
  } else if (event.type === "lat_stats") {
    state.latency = {
      p50: event.p50_ns, p90: event.p90_ns, p99: event.p99_ns,
      max: event.max_ns, n: event.n, label: event.label, ts: event.ts
    };
  }

  const routingKey = event.type === "order"
    ? `tele.order.${symbolName(event.symbolId)}`
    : `tele.${event.type}.global`;
  publishAmqp("hft.telemetry", routingKey, event);
  maybeBroadcastSnapshot(event);

  // High-frequency events (order/ack) are NOT persisted -- Postgres can't keep
  // up; we persist only low-rate types in the background so the handler returns.
  if (event.type === "health" || event.type === "mm_stats") {
    persistEvent(event).catch((err) => {
      console.error("[control] postgres write failed:", err.message);
    });
  }
}

function createTelemetryServer() {
  fs.mkdirSync(path.dirname(TELEMETRY_SOCKET), { recursive: true });
  try {
    fs.unlinkSync(TELEMETRY_SOCKET);
  } catch (err) {
    if (err.code !== "ENOENT") throw err;
  }

  const server = net.createServer((socket) => {
    let buffer = "";
    socket.on("data", (chunk) => {
      buffer += chunk.toString("utf8");
      let idx = buffer.indexOf("\n");
      while (idx >= 0) {
        const line = buffer.slice(0, idx).trim();
        buffer = buffer.slice(idx + 1);
        if (line) {
          try {
            const payload = JSON.parse(line);
            handleTelemetryEvent(payload).catch((err) => {
              console.error("[control] telemetry error:", err.message);
            });
          } catch (err) {
            console.error("[control] bad telemetry line:", err.message);
          }
        }
        idx = buffer.indexOf("\n");
      }
    });
  });

  server.listen(TELEMETRY_SOCKET, () => {
    console.log(`[control] telemetry socket listening at ${TELEMETRY_SOCKET}`);
  });
}

function readBody(req) {
  return new Promise((resolve, reject) => {
    let body = "";
    req.on("data", (chunk) => {
      body += chunk.toString("utf8");
    });
    req.on("end", () => resolve(body));
    req.on("error", reject);
  });
}

function sendJson(res, statusCode, payload) {
  res.writeHead(statusCode, { "content-type": "application/json" });
  res.end(JSON.stringify(payload));
}

function parseLocateBody(body) {
  let payload = {};
  try {
    payload = body ? JSON.parse(body) : {};
  } catch (err) {
    throw new Error("invalid JSON body");
  }

  const locate = Number(payload.locate);
  if (!Number.isInteger(locate) || locate <= 0) {
    throw new Error("locate must be a positive integer");
  }
  return locate;
}

function sendCommand(command) {
  return new Promise((resolve, reject) => {
    const socket = net.createConnection({ path: CONTROL_SOCKET }, () => {
      socket.end(`${command}\n`);
      resolve();
    });
    socket.on("error", reject);
  });
}

async function handleCommand(res, command) {
  try {
    await sendCommand(command);
    if (command === "kill_global") state.requestedGlobalKill = true;
    else if (command === "arm_all") state.requestedGlobalKill = false;
    writeAuditRecord("command", { command, ts: Date.now() });
    publishAmqp("hft.control", `control.${command.replace(" ", ".")}`, {
      command,
      ts: Date.now()
    });
    sendJson(res, 202, { ok: true, command });
  } catch (err) {
    sendJson(res, 503, { ok: false, error: err.message });
  }
}

function serveStatic(req, res, pathname) {
  const filePath = pathname === "/"
    ? path.join(DASHBOARD_DIR, "index.html")
    : path.join(DASHBOARD_DIR, pathname.replace(/^\/+/, ""));
  if (!filePath.startsWith(DASHBOARD_DIR)) {
    res.writeHead(403);
    res.end("forbidden");
    return;
  }

  fs.readFile(filePath, (err, data) => {
    if (err) {
      res.writeHead(404);
      res.end("not found");
      return;
    }
    const ext = path.extname(filePath);
    const contentType = ({
      ".html": "text/html; charset=utf-8",
      ".js": "application/javascript; charset=utf-8",
      ".css": "text/css; charset=utf-8"
    })[ext] || "application/octet-stream";
    // Don't let the browser cache the dashboard -- a stale app.js is a frequent
    // "didn't update" failure mode, and the payload is tiny anyway.
    res.writeHead(200, {
      "content-type": contentType,
      "cache-control": "no-cache, no-store, must-revalidate",
      "pragma":        "no-cache",
      "expires":       "0"
    });
    res.end(data);
  });
}

function createHttpServer() {
  const server = http.createServer(async (req, res) => {
    const url = new URL(req.url, `http://${req.headers.host}`);

    if (req.method === "GET" && url.pathname === "/events") {
      res.writeHead(200, {
        "content-type": "text/event-stream",
        "cache-control": "no-cache",
        connection: "keep-alive"
      });
      res.write(`data: ${JSON.stringify({ kind: "bootstrap", snapshot: snapshotState() })}\n\n`);
      clients.add(res);
      req.on("close", () => clients.delete(res));
      return;
    }

    if (req.method === "GET" && url.pathname === "/api/state") {
      sendJson(res, 200, snapshotState());
      return;
    }

    if (req.method === "POST" && url.pathname === "/api/command/kill-global") {
      await handleCommand(res, "kill_global");
      return;
    }

    if (req.method === "POST" && url.pathname === "/api/command/arm-all") {
      await handleCommand(res, "arm_all");
      return;
    }

    if (req.method === "POST" && url.pathname === "/api/command/kill-symbol") {
      try {
        const locate = parseLocateBody(await readBody(req));
        await handleCommand(res, `kill_symbol ${locate}`);
      } catch (err) {
        sendJson(res, 400, { ok: false, error: err.message });
      }
      return;
    }

    if (req.method === "POST" && url.pathname === "/api/command/arm-symbol") {
      try {
        const locate = parseLocateBody(await readBody(req));
        await handleCommand(res, `arm_symbol ${locate}`);
      } catch (err) {
        sendJson(res, 400, { ok: false, error: err.message });
      }
      return;
    }

    serveStatic(req, res, url.pathname);
  });

  server.listen(PORT, () => {
    console.log(`[control] http listening on :${PORT}`);
  });
}

async function main() {
  ensureAuditStream();
  writeAuditRecord("service_start", {
    port: PORT,
    telemetrySocket: TELEMETRY_SOCKET,
    controlSocket: CONTROL_SOCKET
  });
  await ensureSchema();
  createTelemetryServer();
  createHttpServer();
  connectRabbit().catch((err) => {
    console.error("[control] rabbit init failed:", err.message);
  });
}

main().catch((err) => {
  console.error(err);
  process.exit(1);
});
