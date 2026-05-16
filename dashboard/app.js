const state = {
  requestedGlobalKill: false,
  latestHealth: null,
  recentOrders: [],
  recentAcks: [],
  symbols: [],
  feed: {
    lastSeq: 0,
    packets: 0,
    messages: 0
  },
  totals: {
    activeQuotes: 0,
    orders: 0,
    realizedPnlTicks: 0,
    unrealizedPnlTicks: 0,
    fills: 0
  },
  latency: null,
  snapshotAt: null
};

function fmtInt(value) {
  return Number(value || 0).toLocaleString();
}

function fmtPct(value, digits = 1) {
  if (!Number.isFinite(value)) return "-";
  return `${value.toFixed(digits)}%`;
}

function fmtNs(value) {
  if (!value) return "-";
  const v = Number(value);
  if (v >= 1_000_000) return `${(v / 1_000_000).toFixed(2)} ms`;
  if (v >= 1_000) return `${(v / 1_000).toFixed(1)} us`;
  return `${v} ns`;
}

function fmtTicksUsd(ticks) {
  const usd = Number(ticks || 0) / 100;
  const sign = usd > 0 ? "+" : "";
  return `${sign}${usd.toFixed(2)}`;
}

function fmtSignedInt(value) {
  const n = Number(value || 0);
  const sign = n > 0 ? "+" : "";
  return `${sign}${fmtInt(n)}`;
}

function priceFromTicks(value) {
  if (!value) return "-";
  return (Number(value) / 100).toFixed(2);
}

function fmtTime(value) {
  if (!value) return "-";
  const date = new Date(Number(value));
  if (Number.isNaN(date.getTime())) return String(value);
  return date.toLocaleTimeString([], {
    hour12: false,
    hour: "2-digit",
    minute: "2-digit",
    second: "2-digit"
  });
}

function escapeHtml(value) {
  return String(value)
    .replaceAll("&", "&amp;")
    .replaceAll("<", "&lt;")
    .replaceAll(">", "&gt;");
}

function pnlClass(value) {
  const n = Number(value || 0);
  if (n > 0) return "tone-pos";
  if (n < 0) return "tone-neg";
  return "tone-neutral";
}

function inventoryClass(value) {
  const n = Number(value || 0);
  if (n > 0) return "inventory-long";
  if (n < 0) return "inventory-short";
  return "inventory-flat";
}

function statusForSymbol(symbol) {
  const mm = symbol.mm || {};
  const bid = Number(symbol.ourBidPx || 0);
  const ask = Number(symbol.ourAskPx || 0);
  const inv = Number(mm.inventory || 0);
  const skipPanic = Number(mm.skip_panic || 0);
  const hardBand = Number(mm.skip_hard_band || 0);

  if (skipPanic > 0 && !bid && !ask) {
    return { label: "Panic", className: "status-risk" };
  }
  if (hardBand > 0 || Math.abs(inv) >= 400) {
    return { label: "Inventory", className: "status-watch" };
  }
  if (bid && ask) {
    return { label: "Quoting", className: "status-live" };
  }
  return { label: "One Side", className: "status-watch" };
}

function ackFlag(ack) {
  const flag = Number(ack.flag || 0);
  if (flag === 0) return { label: "OK", className: "tape-ok" };
  if (flag === 1) return { label: "Bad Csum", className: "tape-bad" };
  if (flag === 2) return { label: "Seq Gap", className: "tone-warn" };
  return { label: `Flag ${flag}`, className: "tone-neutral" };
}

function deriveDeskMetrics() {
  const health = state.latestHealth || {};
  const symbols = state.symbols || [];
  const totals = state.totals || {};
  const lat = state.latency || {};

  let netInventory = 0;
  let grossInventory = 0;
  let quotedBoth = 0;
  let oneSided = 0;
  let panicHeld = 0;
  let hardBand = 0;
  let symbolsRateLimited = 0;
  let totalQuotes = 0;
  let totalFills = 0;
  let totalRateLimited = 0;
  let spreadCount = 0;
  let totalSpreadTicks = 0;

  let worstInventory = null;
  let widestSpread = null;
  let bestPnl = null;
  let worstPnl = null;

  for (const symbol of symbols) {
    const mm = symbol.mm || {};
    const bid = Number(symbol.ourBidPx || 0);
    const ask = Number(symbol.ourAskPx || 0);
    const inventory = Number(mm.inventory || 0);
    const realized = Number(mm.realized_pnl_ticks || 0);
    const unrealized = Number(mm.unrealized_pnl_ticks || 0);
    const totalPnl = realized + unrealized;
    const fills = Number(mm.fills || 0);
    const quotes = Number(mm.quotes || 0);
    const rateLimited = Number(mm.rate_limited || 0);
    const spread = bid && ask ? ask - bid : 0;

    netInventory += inventory;
    grossInventory += Math.abs(inventory);
    totalQuotes += quotes;
    totalFills += fills;
    totalRateLimited += rateLimited;

    if (bid && ask) quotedBoth += 1;
    else if (bid || ask) oneSided += 1;

    if (Number(mm.skip_panic || 0) > 0 && !bid && !ask) panicHeld += 1;
    if (Number(mm.skip_hard_band || 0) > 0) hardBand += 1;
    if (rateLimited > 0) symbolsRateLimited += 1;

    if (spread > 0) {
      spreadCount += 1;
      totalSpreadTicks += spread;
      if (!widestSpread || spread > widestSpread.spread) {
        widestSpread = { symbol: symbol.symbol, spread };
      }
    }

    if (!worstInventory || Math.abs(inventory) > Math.abs(worstInventory.inventory)) {
      worstInventory = { symbol: symbol.symbol, inventory };
    }
    if (!bestPnl || totalPnl > bestPnl.pnl) {
      bestPnl = { symbol: symbol.symbol, pnl: totalPnl };
    }
    if (!worstPnl || totalPnl < worstPnl.pnl) {
      worstPnl = { symbol: symbol.symbol, pnl: totalPnl };
    }
  }

  const ackTotal = Number(health.acks || 0);
  const ackOk = Number(health.ackOk || 0);
  const ackExceptions = Number(health.ackBadCsum || 0) + Number(health.ackSeqGap || 0);

  return {
    quotedBoth,
    oneSided,
    panicHeld,
    hardBand,
    symbolsRateLimited,
    totalQuotes,
    totalFills,
    totalRateLimited,
    netInventory,
    grossInventory,
    ackOkRate: ackTotal > 0 ? (ackOk / ackTotal) * 100 : 0,
    ackExceptionRate: ackTotal > 0 ? (ackExceptions / ackTotal) * 100 : 0,
    fillsPerOrder: totals.orders > 0 ? (totals.fills / totals.orders) * 100 : 0,
    fillsPerQuote: totalQuotes > 0 ? (totalFills / totalQuotes) * 100 : 0,
    ordersPerFill: totals.fills > 0 ? totals.orders / totals.fills : 0,
    avgSpreadTicks: spreadCount > 0 ? totalSpreadTicks / spreadCount : 0,
    worstInventory,
    widestSpread,
    bestPnl,
    worstPnl,
    lat
  };
}

function applySnapshot(snapshot) {
  state.requestedGlobalKill = Boolean(snapshot.requestedGlobalKill);
  state.latestHealth = snapshot.latestHealth || null;
  state.recentOrders = snapshot.recentOrders || [];
  state.recentAcks = snapshot.recentAcks || [];
  state.symbols = snapshot.symbols || [];
  state.feed = snapshot.feed || state.feed;
  state.totals = snapshot.totals || state.totals;
  state.latency = snapshot.latency || null;
  state.snapshotAt = Date.now();
  render();
}

function renderHeaderMeta() {
  const health = state.latestHealth || {};
  document.getElementById("last-snapshot").textContent =
    state.snapshotAt ? fmtTime(state.snapshotAt) : "Waiting...";
  document.getElementById("feed-seq").textContent = fmtInt(state.feed.lastSeq || 0);
  document.getElementById("transport-drops").textContent = fmtInt(health.transportDrops || 0);
}

function renderCards() {
  const lat = state.latency;
  const totals = state.totals || {};
  const health = state.latestHealth || {};
  const metrics = deriveDeskMetrics();
  const cards = [
    {
      label: "Requested Kill",
      value: state.requestedGlobalKill ? "ACTIVE" : "ARMED",
      foot: state.requestedGlobalKill ? "Global kill has been requested" : "Desk is live for quoting"
    },
    {
      label: "Feed Throughput",
      value: `${fmtInt(state.feed.messages || 0)}`,
      foot: `${fmtInt(state.feed.packets || 0)} packets observed`
    },
    {
      label: "Execution Latency",
      value: lat ? fmtNs(lat.p50) : "Waiting",
      foot: lat ? `p90 ${fmtNs(lat.p90)} - p99 ${fmtNs(lat.p99)}` : "parse -> send telemetry"
    },
    {
      label: "ACK Quality",
      value: fmtPct(metrics.ackOkRate),
      foot: `${fmtInt(health.acks || 0)} ACKs - ${fmtInt(health.ackSeqGap || 0)} seq gaps`
    },
    {
      label: "Realized PnL",
      value: fmtTicksUsd(totals.realizedPnlTicks),
      foot: `${fmtInt(totals.fills || 0)} fills across the desk`
    },
    {
      label: "Unrealized PnL",
      value: fmtTicksUsd(totals.unrealizedPnlTicks),
      foot: `${fmtInt(totals.activeQuotes || 0)} live quotes`
    },
    {
      label: "Gross Inventory",
      value: fmtInt(metrics.grossInventory),
      foot: `Net ${fmtSignedInt(metrics.netInventory)}`
    },
    {
      label: "Quote Coverage",
      value: `${metrics.quotedBoth}/${state.symbols.length || 0}`,
      foot: `${metrics.oneSided} one-sided - ${metrics.panicHeld} panic-held`
    }
  ];

  document.getElementById("summary-cards").innerHTML = cards.map((card) => `
    <article class="panel card">
      <p class="card-label">${escapeHtml(card.label)}</p>
      <div class="card-value ${escapeHtml(
        card.value.startsWith("+") ? "tone-pos" : card.value.startsWith("-") ? "tone-neg" : ""
      )}">${escapeHtml(card.value)}</div>
      <div class="card-foot">${escapeHtml(card.foot)}</div>
    </article>
  `).join("");
}

function renderDeskOverview() {
  const metrics = deriveDeskMetrics();
  const health = state.latestHealth || {};
  const rows = [
    ["Feed Packets", fmtInt(state.feed.packets || 0)],
    ["Feed Messages", fmtInt(state.feed.messages || 0)],
    ["ACK OK", fmtInt(health.ackOk || 0)],
    ["ACK Exceptions", fmtInt((health.ackBadCsum || 0) + (health.ackSeqGap || 0))],
    ["Active Quotes", fmtInt((state.totals || {}).activeQuotes || 0)],
    ["Avg Live Spread", metrics.avgSpreadTicks > 0 ? priceFromTicks(metrics.avgSpreadTicks) : "-"]
  ];

  document.getElementById("desk-overview").innerHTML = `
    <div class="metric-stack">
      ${rows.map(([name, value]) => `
        <div class="metric-row">
          <span class="metric-name">${escapeHtml(name)}</span>
          <span class="metric-value">${escapeHtml(value)}</span>
        </div>
      `).join("")}
    </div>
  `;
}

function renderExecutionQuality() {
  const metrics = deriveDeskMetrics();
  const rows = [
    [
      "Fill / Order",
      fmtPct(metrics.fillsPerOrder),
      `${fmtInt((state.totals || {}).fills || 0)} fills from ${fmtInt((state.totals || {}).orders || 0)} observed order messages`
    ],
    [
      "Fill / Quote",
      fmtPct(metrics.fillsPerQuote),
      `${fmtInt(metrics.totalFills)} fills from ${fmtInt(metrics.totalQuotes)} strategy quote updates`
    ],
    [
      "Orders / Fill",
      Number.isFinite(metrics.ordersPerFill) && metrics.ordersPerFill > 0
        ? metrics.ordersPerFill.toFixed(1)
        : "-",
      "Lower is cleaner; high values usually mean aggressive requoting or cancel churn"
    ],
    [
      "ACK Success",
      fmtPct(metrics.ackOkRate),
      `${fmtPct(metrics.ackExceptionRate)} exception rate across observer ACKs`
    ],
    [
      "Latency Tail",
      metrics.lat.p99 ? fmtNs(metrics.lat.p99) : "-",
      metrics.lat.max ? `Max ${fmtNs(metrics.lat.max)} - window ${fmtInt(metrics.lat.n || 0)}` : "Waiting for telemetry"
    ],
    [
      "Rate-Limited Msgs",
      fmtInt(metrics.totalRateLimited),
      `${fmtInt(metrics.symbolsRateLimited)} symbols have hit rate limiting`
    ]
  ];

  document.getElementById("execution-quality").innerHTML = `
    <div class="metric-stack">
      ${rows.map(([name, value, subline]) => `
        <div class="metric-row">
          <div>
            <span class="metric-name">${escapeHtml(name)}</span>
            <div class="metric-subline">${escapeHtml(subline)}</div>
          </div>
          <span class="metric-value">${escapeHtml(String(value))}</span>
        </div>
      `).join("")}
    </div>
  `;
}

function renderRiskRadar() {
  const metrics = deriveDeskMetrics();
  const rows = [
    [
      "Net Inventory",
      fmtSignedInt(metrics.netInventory),
      "Directional bias across the full quoted universe"
    ],
    [
      "Gross Inventory",
      fmtInt(metrics.grossInventory),
      "Absolute exposure regardless of side"
    ],
    [
      "Largest Position",
      metrics.worstInventory
        ? `${metrics.worstInventory.symbol} ${fmtSignedInt(metrics.worstInventory.inventory)}`
        : "-",
      "Symbol with the highest absolute live inventory"
    ],
    [
      "Widest Spread",
      metrics.widestSpread
        ? `${metrics.widestSpread.symbol} ${priceFromTicks(metrics.widestSpread.spread)}`
        : "-",
      "Current widest two-sided live market in your universe"
    ],
    [
      "Best / Worst PnL",
      metrics.bestPnl && metrics.worstPnl
        ? `${metrics.bestPnl.symbol} ${fmtTicksUsd(metrics.bestPnl.pnl)} / ${metrics.worstPnl.symbol} ${fmtTicksUsd(metrics.worstPnl.pnl)}`
        : "-",
      "Combined realized + unrealized leaders and laggards"
    ],
    [
      "Risk States",
      `${fmtInt(metrics.oneSided)} one-sided - ${fmtInt(metrics.panicHeld)} panic`,
      `${fmtInt(metrics.hardBand)} hard-band symbols are actively inventory constrained`
    ]
  ];

  document.getElementById("risk-radar").innerHTML = `
    <div class="metric-stack">
      ${rows.map(([name, value, subline]) => `
        <div class="metric-row">
          <div>
            <span class="metric-name">${escapeHtml(name)}</span>
            <div class="metric-subline">${escapeHtml(subline)}</div>
          </div>
          <span class="metric-value">${escapeHtml(String(value))}</span>
        </div>
      `).join("")}
    </div>
  `;
}

function renderTopSymbols() {
  const ranked = [...state.symbols]
    .sort((a, b) => {
      const am = a.mm || {};
      const bm = b.mm || {};
      const ap = Number(am.realized_pnl_ticks || 0) + Number(am.unrealized_pnl_ticks || 0);
      const bp = Number(bm.realized_pnl_ticks || 0) + Number(bm.unrealized_pnl_ticks || 0);
      if (bp !== ap) return bp - ap;
      return Number(b.orders || 0) - Number(a.orders || 0);
    })
    .slice(0, 5);

  if (!ranked.length) {
    document.getElementById("top-symbols").innerHTML =
      `<p class="table-empty">Waiting for symbol telemetry...</p>`;
    return;
  }

  document.getElementById("top-symbols").innerHTML = `
    <div class="leader-list">
      ${ranked.map((symbol) => {
        const mm = symbol.mm || {};
        const pnl = Number(mm.realized_pnl_ticks || 0) + Number(mm.unrealized_pnl_ticks || 0);
        return `
          <div class="leader-row">
            <div>
              <div class="leader-symbol">${escapeHtml(symbol.symbol)}</div>
              <div class="leader-meta">Inv ${mm.inventory || 0} - Fills ${mm.fills || 0} - RL ${mm.rate_limited || 0}</div>
            </div>
            <div class="leader-value ${pnlClass(pnl)}">${escapeHtml(fmtTicksUsd(pnl))}</div>
          </div>
        `;
      }).join("")}
    </div>
  `;
}

function renderSymbols() {
  const body = document.getElementById("symbols-body");
  if (!state.symbols.length) {
    body.innerHTML = `
      <tr>
        <td colspan="13">
          <p class="table-empty">Waiting for live universe state...</p>
        </td>
      </tr>
    `;
    return;
  }

  body.innerHTML = state.symbols.map((symbol) => {
    const mm = symbol.mm || {};
    const bid = Number(symbol.ourBidPx || 0);
    const ask = Number(symbol.ourAskPx || 0);
    const spread = bid && ask ? ask - bid : 0;
    const status = statusForSymbol(symbol);
    return `
      <tr>
        <td>
          <div class="symbol-cell">
            <span class="symbol-main">${escapeHtml(symbol.symbol)}</span>
            <span class="symbol-sub">Locate ${escapeHtml(symbol.locate)}</span>
          </div>
        </td>
        <td><span class="status-chip ${status.className}">${escapeHtml(status.label)}</span></td>
        <td>${priceFromTicks(bid)}</td>
        <td>${priceFromTicks(ask)}</td>
        <td>${spread ? priceFromTicks(spread) : "-"}</td>
        <td>${fmtInt(symbol.orders || 0)}</td>
        <td>${fmtInt(mm.fills || 0)}</td>
        <td class="${inventoryClass(mm.inventory)}">${escapeHtml(String(mm.inventory || 0))}</td>
        <td class="${pnlClass(mm.realized_pnl_ticks)}">${escapeHtml(fmtTicksUsd(mm.realized_pnl_ticks))}</td>
        <td class="${pnlClass(mm.unrealized_pnl_ticks)}">${escapeHtml(fmtTicksUsd(mm.unrealized_pnl_ticks))}</td>
        <td>${fmtInt(mm.rate_limited || 0)}</td>
        <td>${fmtInt(symbol.lastOrderSeq || 0)}</td>
        <td>
          <div class="control-stack">
            <button data-action="kill-symbol" data-locate="${symbol.locate}" class="mini danger">Kill</button>
            <button data-action="arm-symbol" data-locate="${symbol.locate}" class="mini">Arm</button>
          </div>
        </td>
      </tr>
    `;
  }).join("");
}

function renderRecentOrders() {
  const orders = document.getElementById("orders");
  if (!state.recentOrders.length) {
    orders.innerHTML = `<p class="table-empty">No observed orders yet.</p>`;
    return;
  }

  orders.innerHTML = `
    <div class="table-shell">
      <table class="tape-table">
        <thead>
          <tr>
            <th>Time</th>
            <th>Symbol</th>
            <th>Type</th>
            <th>Side</th>
            <th>Px</th>
            <th>Qty</th>
            <th>Seq</th>
          </tr>
        </thead>
        <tbody>
          ${state.recentOrders.map((order) => `
            <tr>
              <td>${escapeHtml(fmtTime(order.ts))}</td>
              <td>${escapeHtml(order.symbol)}</td>
              <td>${escapeHtml(order.msgType)}</td>
              <td class="${order.side === "B" ? "tone-pos" : "tone-neg"}">${escapeHtml(order.side)}</td>
              <td>${priceFromTicks(order.price)}</td>
              <td>${fmtInt(order.qty)}</td>
              <td>${fmtInt(order.seq)}</td>
            </tr>
          `).join("")}
        </tbody>
      </table>
    </div>
  `;
}

function renderRecentAcks() {
  const acks = document.getElementById("acks");
  if (!state.recentAcks.length) {
    acks.innerHTML = `<p class="table-empty">No observed ACKs yet.</p>`;
    return;
  }

  acks.innerHTML = `
    <div class="table-shell">
      <table class="tape-table">
        <thead>
          <tr>
            <th>Time</th>
            <th>Seq</th>
            <th>Status</th>
            <th>Server->Obs</th>
          </tr>
        </thead>
        <tbody>
          ${state.recentAcks.map((ack) => {
            const flag = ackFlag(ack);
            return `
              <tr>
                <td>${escapeHtml(fmtTime(ack.ts))}</td>
                <td>${fmtInt(ack.seq)}</td>
                <td><span class="tape-chip ${flag.className}">${escapeHtml(flag.label)}</span></td>
                <td>${escapeHtml(fmtNs(Number(ack.ts || 0) - Number(ack.recvTs || 0)))}</td>
              </tr>
            `;
          }).join("")}
        </tbody>
      </table>
    </div>
  `;
}

function renderHealth() {
  const health = state.latestHealth;
  const container = document.getElementById("health");
  if (!health) {
    container.innerHTML = `<p class="health-empty">Waiting for observer health...</p>`;
    return;
  }

  const cards = [
    ["Feed Packets", fmtInt(health.feedPackets || 0)],
    ["Feed Messages", fmtInt(health.feedMessages || 0)],
    ["ACK OK", fmtInt(health.ackOk || 0)],
    ["ACK Seq Gap", fmtInt(health.ackSeqGap || 0)],
    ["Bad Csum", fmtInt(health.ackBadCsum || 0)],
    ["Transport Drops", fmtInt(health.transportDrops || 0)],
    ["Observed Orders", fmtInt(health.orders || 0)],
    ["Last ACK Seq", fmtInt(health.lastAckSeq || 0)]
  ];

  container.innerHTML = `
    <div class="health-grid">
      ${cards.map(([label, value]) => `
        <div class="health-card">
          <p class="health-label">${escapeHtml(label)}</p>
          <strong>${escapeHtml(value)}</strong>
        </div>
      `).join("")}
    </div>
    <details class="health-raw">
      <summary>Raw Observer Payload</summary>
      <pre>${escapeHtml(JSON.stringify(health, null, 2))}</pre>
    </details>
  `;
}

function render() {
  renderHeaderMeta();
  renderCards();
  renderDeskOverview();
  renderExecutionQuality();
  renderRiskRadar();
  renderTopSymbols();
  renderSymbols();
  renderRecentOrders();
  renderRecentAcks();
  renderHealth();
}

async function postJson(url, body) {
  const response = await fetch(url, {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: body ? JSON.stringify(body) : "{}"
  });
  if (!response.ok) {
    const payload = await response.json().catch(() => ({ error: response.statusText }));
    throw new Error(payload.error || response.statusText);
  }
}

async function runAction(action) {
  try {
    await action();
  } catch (error) {
    window.alert(error.message);
  }
}

function bindActions() {
  document.getElementById("kill-global").addEventListener("click", () => {
    runAction(() => postJson("/api/command/kill-global"));
  });

  document.getElementById("arm-all").addEventListener("click", () => {
    runAction(() => postJson("/api/command/arm-all"));
  });

  document.body.addEventListener("click", (event) => {
    const button = event.target.closest("button[data-action]");
    if (!button) return;
    const locate = Number(button.dataset.locate);
    const action = button.dataset.action;
    if (action === "kill-symbol") {
      runAction(() => postJson("/api/command/kill-symbol", { locate }));
    } else if (action === "arm-symbol") {
      runAction(() => postJson("/api/command/arm-symbol", { locate }));
    }
  });
}

async function bootstrap() {
  const connection = document.getElementById("connection-state");
  bindActions();

  const initial = await fetch("/api/state").then((response) => response.json());
  applySnapshot(initial);

  const stream = new EventSource("/events");
  stream.onopen = () => {
    connection.textContent = "Live";
    connection.className = "badge ok-badge";
  };
  stream.onerror = () => {
    connection.textContent = "Reconnecting";
    connection.className = "badge warn-badge";
  };
  stream.onmessage = (message) => {
    const payload = JSON.parse(message.data);
    if (payload.snapshot) {
      applySnapshot(payload.snapshot);
    }
  };
}

bootstrap().catch((error) => {
  const connection = document.getElementById("connection-state");
  connection.textContent = error.message;
  connection.className = "badge danger-badge";
});
