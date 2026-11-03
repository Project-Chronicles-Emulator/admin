"use strict";

// Project Chronicles Server Admin interface. All server calls go through the native host ("api" action),
// which adds the admin token; this page never sees it. Player-supplied text (names, log lines)
// is always inserted with textContent, never as HTML.

const POLL_INTERVAL_MS = {
  dashboard: 5000,
  players: 3000,
  logs: 1000,
  // Polled on every page so the sidebar can show unread messages.
  chat: 1500,
  clock: 1000,
};
const HOST_REQUEST_TIMEOUT_MS = 30000;
const MAXIMUM_LOG_LINES = 3000;
const LOG_PAGE_SIZE = 500;
const MAXIMUM_CHAT_LINES = 2000;
const CHAT_PAGE_SIZE = 500;
const TOAST_DURATION_MS = 4500;

const state = {
  connected: false,
  connectedServer: "",
  connectionAttempt: 0,
  realmDirty: false,
  realmEditRevision: 0,
  realmSaving: false,
  page: "dashboard",
  overview: null,
  // Server clock minus local clock, in seconds, from the latest overview.
  serverClockOffset: 0,
  accounts: [],
  characters: [],
  rates: null,
  ratesRevision: 0,
  ratesSaving: false,
  rateDraftDirty: false,
  rateEditRevision: 0,
  logs: {
    after: 0,
    paused: false,
    entries: [],
    inFlight: false,
  },
  chat: {
    after: 0,
    paused: false,
    messages: [],
    unread: 0,
    inFlight: false,
    available: true,
  },
  busy: {
    dashboard: false,
    players: false,
  },
  announceHistory: [],
  mailDelivery: null,
  mailSending: false,
  messageSettingsLoaded: false,
};

// ------------------------------------------------------------------------------------------
// DOM helpers

function byId(id) {
  return document.getElementById(id);
}

// el("td", { className: "x", text: "hello", onClick: fn, title: "..." }, child, child...)
function el(tag, properties = {}, ...children) {
  const node = document.createElement(tag);
  for (const [key, value] of Object.entries(properties)) {
    if (value === undefined || value === null) {
      continue;
    }
    if (key === "text") {
      node.textContent = String(value);
    } else if (key === "className") {
      node.className = value;
    } else if (key.startsWith("on") && typeof value === "function") {
      node.addEventListener(key.slice(2).toLowerCase(), value);
    } else if (key === "dataset") {
      Object.assign(node.dataset, value);
    } else if (key in node && typeof value !== "string") {
      node[key] = value;
    } else {
      node.setAttribute(key, value);
    }
  }
  for (const child of children) {
    if (child === null || child === undefined || child === false) {
      continue;
    }
    node.append(child instanceof Node ? child : document.createTextNode(String(child)));
  }
  return node;
}

function clear(node) {
  while (node.firstChild) {
    node.removeChild(node.firstChild);
  }
}

function badge(text, color) {
  return el("span", { className: "badge badge-" + color, text });
}

function emptyRow(columns, message) {
  return el("tr", {}, el("td", { className: "empty", colSpan: columns, text: message }));
}

// ------------------------------------------------------------------------------------------
// Formatting

function formatNumber(value) {
  return Number(value || 0).toLocaleString();
}

function formatRate(percent) {
  return (percent / 100).toFixed(percent % 100 === 0 ? 1 : 2) + "x";
}

function formatDuration(totalSeconds) {
  const seconds = Math.max(0, Math.floor(totalSeconds));
  const days = Math.floor(seconds / 86400);
  const hours = Math.floor((seconds % 86400) / 3600);
  const minutes = Math.floor((seconds % 3600) / 60);
  if (days > 0) {
    return `${days}d ${hours}h ${minutes}m`;
  }
  if (hours > 0) {
    return `${hours}h ${minutes}m`;
  }
  return `${minutes}m ${seconds % 60}s`;
}

function formatUnix(unixSeconds) {
  if (!unixSeconds) {
    return "--";
  }
  return new Date(unixSeconds * 1000).toLocaleString();
}

function formatTimeOfDay(unixMilliseconds) {
  const date = new Date(unixMilliseconds);
  const pad = (value, width = 2) => String(value).padStart(width, "0");
  return (
    `${pad(date.getHours())}:${pad(date.getMinutes())}:${pad(date.getSeconds())}.` +
    pad(date.getMilliseconds(), 3)
  );
}

function formatPosition(position) {
  if (!position) {
    return "--";
  }
  const meters = (millimeters) => (millimeters / 1000).toFixed(1);
  return `map ${position.map_id} (${meters(position.x_mm)}, ${meters(position.y_mm)}, ${meters(position.z_mm)})`;
}

// datetime-local inputs use local time without a zone.
function unixToLocalInput(unixSeconds) {
  if (!unixSeconds) {
    return "";
  }
  const date = new Date(unixSeconds * 1000);
  const offsetMs = date.getTimezoneOffset() * 60000;
  return new Date(date.getTime() - offsetMs).toISOString().slice(0, 16);
}

function localInputToUnix(text) {
  if (!text) {
    return 0;
  }
  return Math.floor(new Date(text).getTime() / 1000);
}

function stateColor(realmState) {
  switch (realmState) {
    case "normal":
      return "green";
    case "congested":
      return "yellow";
    case "full":
      return "orange";
    case "maintenance":
      return "red";
    default:
      return "muted";
  }
}

// ------------------------------------------------------------------------------------------
// Native host bridge

const pendingRequests = new Map();
let nextRequestId = 1;

function send(action, payload = {}) {
  window.chrome.webview.postMessage({ action, payload });
}

function hostRequest(action, payload) {
  const requestId = nextRequestId++;
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => {
      pendingRequests.delete(requestId);
      reject({
        status: 0,
        error: "timeout",
        message: "The console host did not answer in time",
      });
    }, HOST_REQUEST_TIMEOUT_MS);
    pendingRequests.set(requestId, { resolve, reject, timer });
    send(action, { ...payload, request_id: requestId });
  });
}

function api(method, path, body) {
  return hostRequest("api", {
    method,
    path: "/admin/v1/" + path,
    body: body || {},
  });
}

function handleHostMessage(message) {
  if (!message || typeof message !== "object") {
    return;
  }

  if (message.type === "settings") {
    applySavedSettings(message.settings);
    return;
  }

  if (message.type === "api_result" || message.type === "connect_result") {
    const pending = pendingRequests.get(message.request_id);
    if (!pending) {
      return;
    }
    pendingRequests.delete(message.request_id);
    clearTimeout(pending.timer);
    if (message.ok) {
      pending.resolve(message.result);
    } else {
      if (message.failure && message.failure.status === 401) {
        setConnected(false, "Token rejected");
      }
      pending.reject(message.failure || { message: "Unknown failure" });
    }
  }
}

// ------------------------------------------------------------------------------------------
// Feedback: toasts and dialogs

function toast(message, kind = "success") {
  const node = el("div", { className: "toast toast-" + kind, text: message });
  byId("toasts").append(node);
  setTimeout(() => node.remove(), TOAST_DURATION_MS);
}

function showError(failure) {
  const message = (failure && failure.message) || "The request failed";
  toast(message, "error");
}

// Resolves with the field values when confirmed, or null when cancelled.
// fields: [{ id, label, type, value, options, min, max, maxLength, required, hint }]
function formDialog({ title, description, fields = [], confirmLabel = "Confirm", danger = false }) {
  const dialog = byId("modal");
  const body = byId("modalBody");
  clear(body);
  byId("modalTitle").textContent = title;

  if (description) {
    body.append(el("p", { className: "muted", text: description }));
  }

  const inputs = {};
  for (const field of fields) {
    let input;
    if (field.type === "select") {
      input = el("select", {});
      for (const option of field.options) {
        input.append(el("option", { value: option.value, text: option.label }));
      }
      input.value = field.value;
    } else if (field.type === "checkbox") {
      input = el("input", { type: "checkbox" });
      input.checked = Boolean(field.value);
    } else if (field.type === "textarea") {
      input = el("textarea", { rows: 3 });
      input.value = field.value || "";
    } else {
      input = el("input", { type: field.type || "text" });
      input.value = field.value === undefined || field.value === null ? "" : field.value;
    }
    if (field.min !== undefined) {
      input.min = field.min;
    }
    if (field.max !== undefined) {
      input.max = field.max;
    }
    if (field.maxLength !== undefined) {
      input.maxLength = field.maxLength;
    }
    if (field.step !== undefined) {
      input.step = field.step;
    }
    input.required = Boolean(field.required);
    inputs[field.id] = input;

    const label =
      field.type === "checkbox"
        ? el("label", { className: "inline-check" }, input, field.label)
        : el("label", {}, field.label, input);
    body.append(label);
    if (field.hint) {
      body.append(el("div", { className: "muted small", text: field.hint }));
    }
  }

  const confirmButton = byId("modalConfirm");
  confirmButton.textContent = confirmLabel;
  confirmButton.className = "button " + (danger ? "button-danger" : "button-primary");

  return new Promise((resolve) => {
    const form = byId("modalForm");
    const cancel = byId("modalCancel");

    const finish = (result) => {
      form.removeEventListener("submit", onSubmit);
      cancel.removeEventListener("click", onCancel);
      dialog.removeEventListener("cancel", onCancel);
      dialog.close();
      resolve(result);
    };
    const onSubmit = (event) => {
      event.preventDefault();
      if (!form.reportValidity()) {
        return;
      }
      const values = {};
      for (const [id, input] of Object.entries(inputs)) {
        values[id] = input.type === "checkbox" ? input.checked : input.value;
      }
      finish(values);
    };
    const onCancel = (event) => {
      event.preventDefault();
      finish(null);
    };

    form.addEventListener("submit", onSubmit);
    cancel.addEventListener("click", onCancel);
    dialog.addEventListener("cancel", onCancel);
    dialog.showModal();
    const first = Object.values(inputs)[0];
    if (first) {
      first.focus();
    }
  });
}

async function confirmAction(title, description, confirmLabel = "Confirm", danger = false) {
  const result = await formDialog({ title, description, confirmLabel, danger });
  return result !== null;
}

// Runs an API call and reports failures; resolves to the result or null.
async function attempt(promise, successMessage) {
  try {
    const result = await promise;
    if (successMessage) {
      toast(successMessage);
    }
    return result;
  } catch (failure) {
    showError(failure);
    return null;
  }
}

// ------------------------------------------------------------------------------------------
// Navigation, drawer and connection state

function navigate(page) {
  state.page = page;
  document.querySelectorAll(".nav-item").forEach((button) => {
    button.classList.toggle("active", button.dataset.page === page);
  });
  document.querySelectorAll(".page").forEach((section) => {
    section.classList.toggle("active", section.id === "page-" + page);
  });
  closeDrawer();
  if (page === "chat") {
    markChatRead();
  }
  loadPage(page);
}

function loadPage(page) {
  if (!state.connected && page !== "server") {
    return;
  }
  switch (page) {
    case "dashboard":
      refreshOverview();
      break;
    case "realm":
      loadRealmForm();
      break;
    case "accounts":
      loadAccounts();
      break;
    case "characters":
      loadCharacters();
      break;
    case "players":
      loadPlayers();
      break;
    case "rates":
      loadRates();
      break;
    case "mail":
      if (state.overview && !byId("mailAccount").value) {
        byId("mailWorld").value = state.overview.realm.world_id;
      }
      break;
    case "announce":
      loadMessageSettings();
      break;
    case "logs":
      refreshServerLogLevel();
      break;
    default:
      break;
  }
}

function openDrawer(title, content) {
  byId("drawerTitle").textContent = title;
  const body = byId("drawerBody");
  clear(body);
  body.append(content);
  byId("drawer").classList.remove("hidden");
}

function closeDrawer() {
  byId("drawer").classList.add("hidden");
}

function setConnected(connected, label) {
  state.connected = connected;
  const connectionBadge = byId("connectionBadge");
  connectionBadge.textContent = label || (connected ? "Connected" : "Disconnected");
  connectionBadge.className = "badge " + (connected ? "badge-green" : "badge-red");
  if (!connected) {
    byId("realmBadge").classList.add("hidden");
  }
}

function applySavedSettings(settings) {
  const choice = byId("adminServerChoice");
  clear(choice);
  for (const server of settings.servers || []) {
    const option = document.createElement("option");
    option.value = server.id;
    option.textContent = server.name;
    option.dataset.origin = server.server_url;
    choice.append(option);
  }
  const custom = document.createElement("option");
  custom.value = "custom";
  custom.textContent = "Advanced: custom connection";
  choice.append(custom);
  choice.value = settings.server_id || settings.servers?.[0]?.id || "custom";
  byId("connectUrl").value = settings.server_url || settings.default_server_url || "";
  byId("connectTokenFile").value = settings.token_file || settings.default_token_file || "";
  byId("connectDefaults").dataset.url = settings.default_server_url || "";
  byId("connectDefaults").dataset.tokenFile = settings.default_token_file || "";
  return connect(true);
}

function updateConnectionFields() {
  const custom = byId("adminServerChoice").value === "custom";
  byId("customConnectionFields").classList.toggle("hidden", !custom);
  byId("connectionPresetHint").classList.toggle("hidden", custom);
  byId("connectUrl").required = custom;
  byId("connectSubmit").textContent = custom ? "Connect" : "Reconnect";
}

async function connect(silent = false) {
  const attempt = ++state.connectionAttempt;
  const choice = byId("adminServerChoice");
  const custom = choice.value === "custom";
  updateConnectionFields();
  const origin = custom ? byId("connectUrl").value : choice.selectedOptions[0].dataset.origin;
  const payload = { server_id: choice.value };
  if (custom) {
    payload.server_url = origin;
    payload.token_file = byId("connectTokenFile").value;
    payload.token = byId("connectToken").value;
  }
  setConnected(false, "Connecting...");
  byId("connectSubmit").disabled = true;
  try {
    const overview = await hostRequest("connect", payload);
    if (attempt !== state.connectionAttempt) {
      return;
    }
    byId("connectToken").value = "";
    setConnected(true);
    state.connectedServer = origin.trim();
    state.rates = null;
    state.ratesRevision += 1;
    state.ratesSaving = false;
    state.rateDraftDirty = false;
    state.busy.dashboard = false;
    setRateControlsBusy(false);
    state.realmDirty = false;
    state.realmEditRevision += 1;
    state.messageSettingsLoaded = false;
    byId("messageConfigSave").disabled = true;
    renderOverview(overview);
    state.logs.after = 0;
    resetChat();
    if (!silent) {
      toast("Connected to " + overview.realm.world_name);
    }
    loadPage(state.page);
  } catch (failure) {
    if (attempt !== state.connectionAttempt) {
      return;
    }
    setConnected(false, "Connection failed");
    if (silent) {
      navigate("server");
      toast("Not connected: " + (failure.message || "server unavailable"), "error");
    } else {
      showError(failure);
    }
  } finally {
    if (attempt === state.connectionAttempt) {
      byId("connectSubmit").disabled = false;
    }
  }
}

// ------------------------------------------------------------------------------------------
// Dashboard

async function refreshOverview() {
  if (!state.connected || state.busy.dashboard) {
    return state.overview;
  }
  const connection = state.connectionAttempt;
  state.busy.dashboard = true;
  try {
    const overview = await api("GET", "overview");
    if (connection !== state.connectionAttempt) {
      return state.overview;
    }
    renderOverview(overview);
    return overview;
  } catch (failure) {
    if (connection === state.connectionAttempt && failure.error === "server_unreachable") {
      setConnected(false, "Server unreachable");
    }
    return state.overview;
  } finally {
    if (connection === state.connectionAttempt) {
      state.busy.dashboard = false;
    }
  }
}

function renderOverview(overview) {
  state.overview = overview;
  const { realm, population, rates, server } = overview;
  state.serverClockOffset = server.server_time_unix - Date.now() / 1000;

  byId("dashRealmName").textContent = realm.world_name;
  const realmState = byId("dashRealmState");
  realmState.textContent = realm.state;
  realmState.className = "badge badge-" + stateColor(realm.state);
  byId("dashOnline").textContent = formatNumber(realm.online);
  byId("dashCapacity").textContent = formatNumber(realm.capacity);
  const usedPercent = Math.min(100, (realm.online / Math.max(1, realm.capacity)) * 100);
  const bar = byId("dashCapacityBar");
  bar.style.width = usedPercent + "%";
  let barColor = "var(--green)";
  if (usedPercent >= 100) {
    barColor = "var(--orange)";
  } else if (usedPercent >= 80) {
    barColor = "var(--yellow)";
  }
  bar.style.background = barColor;
  byId("dashMaintenance").checked = realm.maintenance;
  byId("dashCreation").checked = realm.char_creation_restricted;

  const realmBadge = byId("realmBadge");
  realmBadge.textContent = `${realm.world_name} - ${realm.state}`;
  realmBadge.className = "badge badge-" + stateColor(realm.state);

  byId("dashInWorld").textContent = formatNumber(population.in_world);
  byId("dashConnections").textContent = formatNumber(population.world_connections);
  byId("dashLobby").textContent = formatNumber(population.lobby_sessions);
  byId("dashAccounts").textContent = formatNumber(population.accounts);
  byId("dashCharacters").textContent = formatNumber(population.characters);

  byId("dashExpRate").textContent = formatRate(rates.effective_experience_percent);
  byId("dashExpBase").textContent = rates.experience_rate_percent;
  const activeList = byId("dashActiveEvents");
  clear(activeList);
  const activeEvents = rates.events.filter((event) => event.active);
  if (activeEvents.length === 0) {
    activeList.append(el("li", { className: "muted", text: "No active events" }));
  }
  for (const event of activeEvents) {
    activeList.append(
      el("li", {}, badge("+" + event.experience_bonus_percent + "%", "green"), " ", event.name),
    );
  }

  byId("dashUptime").textContent = formatDuration(server.uptime_seconds);
  byId("dashStarted").textContent = formatUnix(server.started_at_unix);
  byId("dashStorage").textContent = server.storage_backend;
  byId("dashPorts").textContent =
    `${server.http_port} / ${server.world_listener ? server.world_port : "off"}`;
  byId("dashClient").textContent = server.client_version || "--";
  byId("dashLogLevel").textContent = server.log_level;
  byId("dashPersisted").textContent = server.settings_persisted ? "yes" : "until restart only";
  byId("serverLogLevel").value = server.log_level === "warn" ? "warning" : server.log_level;
}

async function quickRealmToggle(field, checkbox) {
  const result = await attempt(
    api("POST", "realm", { [field]: checkbox.checked }),
    "Realm updated",
  );
  if (!result) {
    checkbox.checked = !checkbox.checked;
  }
  refreshOverview();
}

function tickClock() {
  if (!state.overview) {
    return;
  }
  const serverNow = Date.now() / 1000 + state.serverClockOffset;
  byId("serverClock").textContent = new Date(serverNow * 1000).toLocaleTimeString();
}

// ------------------------------------------------------------------------------------------
// Realm

function fillRealmForm() {
  if (state.realmDirty || !state.overview) {
    return;
  }
  const realm = state.overview.realm;
  byId("realmName").value = realm.world_name;
  byId("realmSite").value = realm.site_display_name;
  byId("realmCapacity").value = realm.capacity;
  byId("realmLabel").value = realm.label;
  byId("realmMaintenance").checked = realm.maintenance;
  byId("realmCreation").checked = realm.char_creation_restricted;
}

function markRealmEdited() {
  state.realmDirty = true;
  state.realmEditRevision += 1;
  byId("realmSaveStatus").textContent = "Unsaved changes";
}

async function loadRealmForm() {
  // Show cached values immediately. A slow remote reply must not erase typing.
  fillRealmForm();
  const connection = state.connectionAttempt;
  await refreshOverview();
  if (connection === state.connectionAttempt && state.page === "realm") {
    fillRealmForm();
  }
}

async function saveRealm(event) {
  event.preventDefault();
  if (state.realmSaving) {
    return;
  }
  const capacityText = byId("realmCapacity").value.trim();
  const capacity = Number(capacityText);
  if (!/^[0-9]{1,6}$/u.test(capacityText) || capacity < 1 || capacity > 100000) {
    byId("realmSaveStatus").textContent = "Enter a whole-number capacity from 1 to 100000.";
    return;
  }
  const connection = state.connectionAttempt;
  const revision = state.realmEditRevision;
  const body = {
    world_name: byId("realmName").value.trim(),
    site_display_name: byId("realmSite").value.trim(),
    capacity,
    label: byId("realmLabel").value,
    maintenance: byId("realmMaintenance").checked,
    char_creation_restricted: byId("realmCreation").checked,
  };
  state.realmSaving = true;
  byId("realmSave").disabled = true;
  byId("realmSaveStatus").textContent = "Saving realm... You can keep editing.";
  try {
    const result = await api("POST", "realm", body);
    if (connection !== state.connectionAttempt) {
      return;
    }
    if (state.overview) {
      Object.assign(state.overview.realm, result);
    }
    const persisted = Boolean(result.settings_saved);
    const message = persisted ? "Realm saved" : "Realm applied, but not saved for restart";
    toast(message, persisted ? "success" : "error");
    if (revision === state.realmEditRevision) {
      state.realmDirty = !persisted;
      byId("realmSaveStatus").textContent = message;
      fillRealmForm();
    } else {
      byId("realmSaveStatus").textContent = "Newer edits are not saved yet";
    }
  } catch (failure) {
    if (connection === state.connectionAttempt) {
      byId("realmSaveStatus").textContent = "Save failed. Your edits are still here.";
      showError(failure);
    }
  } finally {
    state.realmSaving = false;
    byId("realmSave").disabled = false;
  }
}

// ------------------------------------------------------------------------------------------
// Accounts

async function loadAccounts() {
  const search = encodeURIComponent(byId("accountSearch").value.trim());
  const result = await attempt(api("GET", "accounts?search=" + search));
  if (!result) {
    return;
  }
  state.accounts = result.accounts;
  renderAccounts(result);
}

function renderAccounts(result) {
  const rows = byId("accountRows");
  clear(rows);
  if (result.accounts.length === 0) {
    rows.append(emptyRow(8, "No accounts match"));
  }
  for (const account of result.accounts) {
    const status = account.banned ? badge("banned", "red") : badge("active", "green");
    const role = account.role === "gm" ? badge("GM", "blue") : el("span", { text: "player" });
    rows.append(
      el(
        "tr",
        { className: "clickable", onClick: () => openAccount(account.id) },
        el("td", { text: account.id }),
        el("td", { text: account.name }),
        el("td", {}, role),
        el("td", {}, status),
        el("td", { text: account.character_count }),
        el("td", { text: account.lobby_sessions }),
        el("td", { text: account.online_characters }),
        el("td", {
          text: account.created_at ? new Date(account.created_at).toLocaleString() : "--",
        }),
      ),
    );
  }
  byId("accountTotal").textContent = `${result.accounts.length} shown of ${result.total} accounts`;
}

async function createAccount() {
  const values = await formDialog({
    title: "New account",
    fields: [
      {
        id: "account",
        label: "Account name (3-32 letters, digits, underscore)",
        required: true,
        maxLength: 32,
      },
      {
        id: "password",
        label: "Password (8+ characters)",
        type: "password",
        required: true,
      },
      {
        id: "role",
        label: "Role",
        type: "select",
        value: "player",
        options: [
          { value: "player", label: "Player" },
          { value: "gm", label: "Game master" },
        ],
      },
    ],
    confirmLabel: "Create",
  });
  if (!values) {
    return;
  }
  const created = await attempt(api("POST", "accounts", values), "Account created");
  if (created) {
    loadAccounts();
  }
}

async function openAccount(accountId) {
  const account = await attempt(api("GET", "accounts/" + accountId));
  if (!account) {
    return;
  }

  const content = el("div", {});
  content.append(
    el(
      "div",
      { className: "drawer-section" },
      el("h3", { text: "Account" }),
      el(
        "dl",
        { className: "facts" },
        el("dt", { text: "ID" }),
        el("dd", { text: account.id }),
        el("dt", { text: "Role" }),
        el("dd", { text: account.role }),
        el("dt", { text: "Status" }),
        el("dd", {}, account.banned ? badge("banned", "red") : badge("active", "green")),
        el("dt", { text: "Lobby sessions" }),
        el("dd", { text: account.lobby_sessions }),
        el("dt", { text: "Characters in world" }),
        el("dd", { text: account.online_characters }),
      ),
    ),
  );

  const actions = el("div", { className: "action-row" });
  actions.append(
    el("button", {
      className: "button",
      text: "Reset password",
      onClick: () => resetPassword(account),
    }),
    el("button", {
      className: "button",
      text: account.role === "gm" ? "Make player" : "Make game master",
      onClick: () => setRole(account, account.role === "gm" ? "player" : "gm"),
    }),
    el("button", {
      className: "button",
      text: "Sign out everywhere",
      onClick: () => kickAccount(account),
    }),
    el("button", {
      className: account.banned ? "button" : "button button-danger",
      text: account.banned ? "Unban" : "Ban",
      onClick: () => setBanned(account, !account.banned),
    }),
  );
  content.append(
    el("div", { className: "drawer-section" }, el("h3", { text: "Actions" }), actions),
  );

  const characterTable = el("tbody", {});
  if (account.characters.length === 0) {
    characterTable.append(emptyRow(4, "No characters"));
  }
  for (const character of account.characters) {
    characterTable.append(
      el(
        "tr",
        { className: "clickable", onClick: () => openCharacter(character.id) },
        el("td", { text: character.name }),
        el("td", { text: character.class_name }),
        el("td", { text: character.level }),
        el("td", {}, character.online ? badge("online", "green") : badge("offline", "muted")),
      ),
    );
  }
  content.append(
    el(
      "div",
      { className: "drawer-section" },
      el("h3", { text: "Characters" }),
      el(
        "table",
        {},
        el(
          "thead",
          {},
          el(
            "tr",
            {},
            el("th", { text: "Name" }),
            el("th", { text: "Class" }),
            el("th", { text: "Level" }),
            el("th", { text: "Status" }),
          ),
        ),
        characterTable,
      ),
    ),
  );

  openDrawer("Account: " + account.name, content);
}

async function resetPassword(account) {
  const values = await formDialog({
    title: "Reset password for " + account.name,
    description: "Every session of this account is signed out.",
    fields: [
      {
        id: "password",
        label: "New password (8+ characters)",
        type: "password",
        required: true,
      },
    ],
    confirmLabel: "Reset password",
  });
  if (!values) {
    return;
  }
  await attempt(api("POST", `accounts/${account.id}/password`, values), "Password reset");
}

async function setRole(account, role) {
  const confirmed = await confirmAction(
    "Change role",
    `Set ${account.name} to ${role === "gm" ? "game master" : "player"}? The change applies at the next login.`,
  );
  if (!confirmed) {
    return;
  }
  const result = await attempt(
    api("POST", `accounts/${account.id}/role`, { role }),
    "Role updated",
  );
  if (result) {
    openAccount(account.id);
    loadAccounts();
  }
}

async function kickAccount(account) {
  const confirmed = await confirmAction(
    "Sign out everywhere",
    `Revoke every session of ${account.name} and disconnect its characters from the world?`,
  );
  if (!confirmed) {
    return;
  }
  const result = await attempt(api("POST", `accounts/${account.id}/kick`));
  if (result) {
    toast(`Signed out; ${result.disconnected} world connection(s) closed`);
    openAccount(account.id);
  }
}

async function setBanned(account, banned) {
  const confirmed = await confirmAction(
    banned ? "Ban account" : "Unban account",
    banned
      ? `Ban ${account.name}? Its sessions are revoked, its characters are disconnected and login is refused.`
      : `Allow ${account.name} to log in again?`,
    banned ? "Ban" : "Unban",
    banned,
  );
  if (!confirmed) {
    return;
  }
  const result = await attempt(api("POST", `accounts/${account.id}/ban`, { banned }));
  if (result) {
    toast(
      banned ? `Banned; ${result.disconnected} world connection(s) closed` : "Account unbanned",
    );
    openAccount(account.id);
    loadAccounts();
  }
}

// ------------------------------------------------------------------------------------------
// Characters

async function loadCharacters() {
  const search = encodeURIComponent(byId("characterSearch").value.trim());
  const result = await attempt(api("GET", "characters?search=" + search));
  if (!result) {
    return;
  }
  state.characters = result.characters;

  const rows = byId("characterRows");
  clear(rows);
  if (result.characters.length === 0) {
    rows.append(emptyRow(8, "No characters match"));
  }
  for (const character of result.characters) {
    const map = character.saved_position ? character.saved_position.map_id : "--";
    rows.append(
      el(
        "tr",
        { className: "clickable", onClick: () => openCharacter(character.id) },
        el("td", { text: character.id }),
        el("td", { text: character.name }),
        el("td", { text: character.account_name }),
        el("td", { text: character.class_name }),
        el("td", { text: character.level }),
        el("td", { text: formatNumber(character.experience) }),
        el("td", { text: map }),
        el("td", {}, character.online ? badge("online", "green") : badge("offline", "muted")),
      ),
    );
  }
  byId("characterTotal").textContent =
    `${result.characters.length} shown of ${result.total} characters`;
}

function factList(pairs) {
  const list = el("dl", { className: "facts" });
  for (const [label, value] of pairs) {
    list.append(el("dt", { text: label }));
    list.append(value instanceof Node ? el("dd", {}, value) : el("dd", { text: value }));
  }
  return list;
}

function miniForm(fields, buttonLabel, onSubmit) {
  const inputs = {};
  const form = el("form", { className: "mini-form" });
  for (const field of fields) {
    const input = el("input", { type: field.type || "number" });
    input.value = field.value === undefined || field.value === null ? "" : field.value;
    if (field.step !== undefined) {
      input.step = field.step;
    }
    if (field.min !== undefined) {
      input.min = field.min;
    }
    input.required = field.required !== false;
    inputs[field.id] = input;
    form.append(el("label", {}, field.label, input));
  }
  form.append(
    el("button", {
      type: "submit",
      className: "button button-primary",
      text: buttonLabel,
    }),
  );
  form.addEventListener("submit", (event) => {
    event.preventDefault();
    const values = {};
    for (const [id, input] of Object.entries(inputs)) {
      values[id] = input.type === "number" ? Number(input.value) : input.value.trim();
    }
    onSubmit(values);
  });
  return form;
}

async function openCharacter(characterId) {
  const character = await attempt(api("GET", "characters/" + characterId));
  if (!character) {
    return;
  }

  const progress = character.progress;
  const editPath = `characters/${character.id}/`;
  const afterEdit = async (result, message) => {
    if (result) {
      toast(message);
      await openCharacter(character.id);
      if (state.page === "characters") {
        loadCharacters();
      }
    }
  };
  const edit = async (action, body, message) => {
    const result = await attempt(api("POST", editPath + action, body));
    await afterEdit(result, message);
  };

  const content = el("div", {});
  if (character.online) {
    content.append(
      el(
        "div",
        { className: "notice" },
        "This character is in the world. Kick it from Live players before editing; the live session owns its state.",
      ),
    );
  }

  content.append(
    el(
      "div",
      { className: "drawer-section" },
      el("h3", { text: "Overview" }),
      factList([
        ["ID", character.id],
        ["Account", character.account_name],
        ["Class", character.class_name],
        ["Level", character.level],
        ["Experience (into level)", formatNumber(character.experience)],
        ["Gold", formatNumber(progress.gold)],
        ["Hersander tokens", formatNumber(progress.hersander_token)],
        ["State", progress.dead ? badge("dead", "red") : badge("alive", "green")],
        ["Saved position", formatPosition(character.saved_position)],
        ["Live position", character.live_position ? formatPosition(character.live_position) : "--"],
        [
          "Quests completed / in progress",
          `${progress.quests_completed} / ${progress.quests_in_progress}`,
        ],
        ["Progress revision", progress.revision],
      ]),
    ),
  );

  const editSection = el(
    "div",
    { className: "drawer-section" },
    el("h3", { text: "Edit (offline only)" }),
  );
  editSection.append(
    miniForm(
      [
        { id: "level", label: "Level", value: character.level, min: 1 },
        {
          id: "experience",
          label: "EXP into level",
          value: character.experience,
          min: 0,
        },
      ],
      "Save progress",
      (values) => edit("progress", values, "Progress saved"),
    ),
    el("br"),
    miniForm(
      [
        { id: "gold", label: "Gold", value: progress.gold, min: 0 },
        {
          id: "hersander_token",
          label: "Hersander tokens",
          value: progress.hersander_token,
          min: 0,
        },
      ],
      "Save currency",
      (values) => edit("currency", values, "Currency saved"),
    ),
    el("br"),
    miniForm(
      [
        {
          id: "map_id",
          label: "Map ID",
          value: character.saved_position ? character.saved_position.map_id : 8000,
          min: 1,
        },
        {
          id: "x_mm",
          label: "X (mm)",
          value: character.saved_position ? character.saved_position.x_mm : 0,
        },
        {
          id: "y_mm",
          label: "Y (mm)",
          value: character.saved_position ? character.saved_position.y_mm : 0,
        },
        {
          id: "z_mm",
          label: "Z (mm)",
          value: character.saved_position ? character.saved_position.z_mm : 0,
        },
        {
          id: "yaw",
          label: "Yaw",
          value: character.saved_position ? character.saved_position.yaw : 0,
          step: "any",
        },
      ],
      "Teleport",
      (values) => edit("position", values, "Position saved"),
    ),
    el("div", {
      className: "muted small",
      text: "World entry only accepts the tutorial map or the server's loaded overworld map.",
    }),
  );
  content.append(editSection);

  const dangerActions = el("div", { className: "action-row" });
  dangerActions.append(
    el("button", {
      className: "button",
      text: "Rename",
      onClick: async () => {
        const values = await formDialog({
          title: "Rename " + character.name,
          fields: [
            {
              id: "name",
              label: "New name (3-16 letters or digits)",
              value: character.name,
              required: true,
              maxLength: 16,
            },
          ],
          confirmLabel: "Rename",
        });
        if (values) {
          await edit("rename", values, "Character renamed");
        }
      },
    }),
    el("button", {
      className: "button button-danger",
      text: "Delete character",
      onClick: async () => {
        const values = await formDialog({
          title: "Delete " + character.name,
          description:
            "This permanently removes the character, its items and its progress. Type the character name to confirm.",
          fields: [{ id: "confirm_name", label: "Character name", required: true }],
          confirmLabel: "Delete",
          danger: true,
        });
        if (!values) {
          return;
        }
        const result = await attempt(api("POST", editPath + "delete", values), "Character deleted");
        if (result) {
          closeDrawer();
          loadCharacters();
        }
      },
    }),
  );
  content.append(
    el("div", { className: "drawer-section" }, el("h3", { text: "Manage" }), dangerActions),
  );

  const itemRows = el("tbody", {});
  if (character.items.length === 0) {
    itemRows.append(emptyRow(6, "No item instances"));
  }
  for (const item of character.items) {
    itemRows.append(
      el(
        "tr",
        {},
        el("td", { text: item.cms_id }),
        el("td", { text: item.inventory_slot }),
        el("td", { text: item.equip_slot === null ? "--" : item.equip_slot }),
        el("td", { text: item.quantity }),
        el("td", { text: item.durability }),
        el("td", { text: item.grade }),
      ),
    );
  }
  for (const stack of progress.stackables) {
    itemRows.append(
      el(
        "tr",
        {},
        el("td", { text: stack.cms_id }),
        el("td", { text: "stack" }),
        el("td", { text: "--" }),
        el("td", { text: stack.count }),
        el("td", { text: "--" }),
        el("td", { text: "--" }),
      ),
    );
  }
  content.append(
    el(
      "div",
      { className: "drawer-section" },
      el("h3", { text: "Items" }),
      el(
        "div",
        { className: "table-wrap" },
        el(
          "table",
          {},
          el(
            "thead",
            {},
            el(
              "tr",
              {},
              el("th", { text: "Item CMS" }),
              el("th", { text: "Slot" }),
              el("th", { text: "Equipped" }),
              el("th", { text: "Qty" }),
              el("th", { text: "Durability" }),
              el("th", { text: "Grade" }),
            ),
          ),
          itemRows,
        ),
      ),
    ),
  );

  openDrawer("Character: " + character.name, content);
}

// ------------------------------------------------------------------------------------------
// Live players

async function loadPlayers() {
  if (!state.connected || state.busy.players) {
    return;
  }
  state.busy.players = true;
  try {
    const result = await api("GET", "players");
    renderPlayers(result);
  } catch (failure) {
    showError(failure);
  } finally {
    state.busy.players = false;
  }
}

function renderPlayers(result) {
  const rows = byId("playerRows");
  clear(rows);
  if (!result.world_listener) {
    rows.append(emptyRow(8, "The world listener is not running (--world-port is not set)"));
    byId("playersNote").textContent = "";
    return;
  }
  if (result.players.length === 0) {
    rows.append(emptyRow(8, "Nobody is connected to the world service"));
  }

  for (const player of result.players) {
    const characterCell = el("td", {});
    if (!player.authenticated) {
      characterCell.append(badge("authenticating", "yellow"));
    } else {
      characterCell.append(player.character_name || "#" + player.character_id);
      if (player.game_master) {
        characterCell.append(" ", badge("GM", "blue"));
      }
    }
    const kick = el("button", {
      className: "button button-small button-danger",
      text: "Kick",
      onClick: async () => {
        const name = player.character_name || "connection " + player.connection_id;
        if (
          !(await confirmAction(
            "Kick player",
            `Disconnect ${name}? Its position is saved first.`,
            "Kick",
            true,
          ))
        ) {
          return;
        }
        await attempt(api("POST", `players/${player.connection_id}/kick`), "Player disconnected");
        setTimeout(loadPlayers, 500);
      },
    });
    rows.append(
      el(
        "tr",
        {},
        el("td", { text: player.connection_id }),
        characterCell,
        el("td", { text: player.account_name || "--" }),
        el("td", {
          text: player.level ? `${player.level} ${player.class_name}` : "--",
        }),
        el("td", { text: formatPosition(player.position) }),
        el("td", { className: "mono", text: player.remote_address || "--" }),
        el("td", { text: formatDuration(player.connected_seconds) }),
        el("td", {}, kick),
      ),
    );
  }
  byId("playersNote").textContent =
    `${result.players.length} world connection(s); updated ${new Date().toLocaleTimeString()}`;
}

// ------------------------------------------------------------------------------------------
// Rates and events

async function loadRates() {
  if (state.ratesSaving) {
    return;
  }
  const connection = state.connectionAttempt;
  const revision = ++state.ratesRevision;
  const cached = state.rates || state.overview?.rates;
  if (cached) {
    renderRates(cached);
  } else {
    byId("ratesEffective").textContent = "Loading...";
    byId("ratesMaximum").textContent = "â€”";
  }
  byId("rateSaveStatus").textContent = "Checking server rates...";
  try {
    const rates = await api("GET", "settings");
    if (connection !== state.connectionAttempt || revision !== state.ratesRevision) {
      return;
    }
    renderRates(rates);
    byId("rateSaveStatus").textContent = "Current rate loaded from this server.";
  } catch (failure) {
    if (connection === state.connectionAttempt && revision === state.ratesRevision) {
      byId("rateSaveStatus").textContent = cached
        ? "Could not refresh. Showing this server's last known rate."
        : "Could not load this server's rate. Retry the connection.";
      showError(failure);
    }
  }
}

function renderRates(rates) {
  state.rates = rates;
  byId("ratesEffective").textContent = formatRate(rates.effective_experience_percent);
  byId("ratesMaximum").textContent = rates.maximum_rate_percent;
  if (!state.rateDraftDirty) {
    byId("rateBase").value = rates.experience_rate_percent;
  }
  document.querySelectorAll("#ratePresets [data-rate]").forEach((button) => {
    const selected = Number(button.dataset.rate) === rates.experience_rate_percent;
    button.classList.toggle("button-primary", selected);
  });

  const rows = byId("eventRows");
  clear(rows);
  if (rates.events.length === 0) {
    rows.append(emptyRow(6, "No events scheduled"));
  }
  for (const event of rates.events) {
    let status = badge("scheduled", "blue");
    if (!event.enabled) {
      status = badge("disabled", "muted");
    } else if (event.active) {
      status = badge("active", "green");
    } else if (event.ends_at_unix && event.ends_at_unix <= rates.server_time_unix) {
      status = badge("ended", "muted");
    }

    rows.append(
      el(
        "tr",
        {},
        el(
          "td",
          {},
          el("div", { text: event.name }),
          event.description
            ? el("div", { className: "muted small", text: event.description })
            : null,
        ),
        el("td", { text: "+" + event.experience_bonus_percent + "%" }),
        el("td", {
          text: event.starts_at_unix ? formatUnix(event.starts_at_unix) : "immediately",
        }),
        el("td", {
          text: event.ends_at_unix ? formatUnix(event.ends_at_unix) : "no end",
        }),
        el("td", {}, status),
        el(
          "td",
          {},
          el(
            "div",
            { className: "action-row" },
            el("button", {
              className: "button button-small",
              text: event.enabled ? "Disable" : "Enable",
              onClick: () => saveEvent(event.id, { ...event, enabled: !event.enabled }),
            }),
            el("button", {
              className: "button button-small",
              text: "Edit",
              onClick: () => editEvent(event),
            }),
            el("button", {
              className: "button button-small button-danger",
              text: "Delete",
              onClick: async () => {
                if (
                  await confirmAction("Delete event", `Delete "${event.name}"?`, "Delete", true)
                ) {
                  const result = await attempt(
                    api("POST", `events/${event.id}/delete`),
                    "Event deleted",
                  );
                  if (result) {
                    renderRates(result);
                  }
                }
              },
            }),
          ),
        ),
      ),
    );
  }
}

function setRateControlsBusy(busy) {
  byId("rateApply").disabled = busy;
  document.querySelectorAll("#ratePresets [data-rate]").forEach((button) => {
    button.disabled = busy;
  });
}

function markRateEdited() {
  state.rateDraftDirty = true;
  state.rateEditRevision += 1;
  byId("rateSaveStatus").textContent = "Unsaved rate change";
}

async function setBaseRate(percent) {
  if (state.ratesSaving) {
    return;
  }
  if (!Number.isInteger(percent) || percent < 0 || percent > 1000) {
    byId("rateSaveStatus").textContent = "Enter a whole percentage from 0 to 1000.";
    return;
  }
  const connection = state.connectionAttempt;
  const revision = ++state.ratesRevision;
  const editRevision = state.rateEditRevision;
  state.ratesSaving = true;
  setRateControlsBusy(true);
  byId("rateSaveStatus").textContent = "Saving base experience rate...";
  try {
    const result = await api("POST", "settings", { experience_rate_percent: percent });
    if (connection !== state.connectionAttempt || revision !== state.ratesRevision) {
      return;
    }
    const persisted = Boolean(result.settings_saved);
    if (editRevision === state.rateEditRevision) {
      state.rateDraftDirty = false;
    }
    renderRates(result);
    const message = persisted
      ? "Base experience rate " + formatRate(percent) + " saved."
      : "Rate applied for this run only; it could not be saved for restart.";
    byId("rateSaveStatus").textContent = state.rateDraftDirty
      ? "Newer rate edits are not saved yet."
      : message;
    toast(message, persisted ? "success" : "error");
    refreshOverview();
  } catch (failure) {
    if (connection === state.connectionAttempt && revision === state.ratesRevision) {
      byId("rateSaveStatus").textContent = "Rate save failed. Your entry is still here.";
      showError(failure);
    }
  } finally {
    if (connection === state.connectionAttempt) {
      state.ratesSaving = false;
      setRateControlsBusy(false);
    }
  }
}

async function eventDialog(title, event) {
  const values = await formDialog({
    title,
    fields: [
      {
        id: "name",
        label: "Name",
        value: event.name || "",
        required: true,
        maxLength: 64,
      },
      {
        id: "description",
        label: "Description (optional)",
        value: event.description || "",
        maxLength: 256,
      },
      {
        id: "experience_bonus_percent",
        label: "EXP bonus (%; +100 doubles the base 1x rate)",
        type: "number",
        value: event.experience_bonus_percent ?? 100,
        min: 0,
        max: 1000,
        required: true,
      },
      {
        id: "starts_at",
        label: "Starts (empty = now)",
        type: "datetime-local",
        value: unixToLocalInput(event.starts_at_unix),
      },
      {
        id: "ends_at",
        label: "Ends (empty = no end)",
        type: "datetime-local",
        value: unixToLocalInput(event.ends_at_unix),
      },
      {
        id: "enabled",
        label: "Enabled",
        type: "checkbox",
        value: event.enabled !== false,
      },
    ],
    confirmLabel: "Save event",
  });
  if (!values) {
    return null;
  }
  return {
    name: values.name.trim(),
    description: values.description.trim(),
    experience_bonus_percent: Number(values.experience_bonus_percent),
    starts_at_unix: localInputToUnix(values.starts_at),
    ends_at_unix: localInputToUnix(values.ends_at),
    enabled: values.enabled,
  };
}

async function createEvent() {
  const event = await eventDialog("New event", {});
  if (!event) {
    return;
  }
  const result = await attempt(api("POST", "events", event), "Event scheduled");
  if (result) {
    renderRates(result);
    refreshOverview();
  }
}

async function editEvent(event) {
  const updated = await eventDialog("Edit event", event);
  if (updated) {
    await saveEvent(event.id, updated);
  }
}

async function saveEvent(eventId, event) {
  const body = {
    name: event.name,
    description: event.description,
    experience_bonus_percent: event.experience_bonus_percent,
    starts_at_unix: event.starts_at_unix,
    ends_at_unix: event.ends_at_unix,
    enabled: event.enabled,
  };
  const result = await attempt(api("POST", "events/" + eventId, body), "Event saved");
  if (result) {
    renderRates(result);
    refreshOverview();
  }
}

// ------------------------------------------------------------------------------------------
// Announcements

async function sendAnnouncement(event) {
  event.preventDefault();
  const text = byId("announceText").value.trim();
  const body = {
    text,
    chat: byId("announceChat").checked,
    chat_sender: byId("announceSender").value.trim() || "[Server]",
    system_notice: byId("announceNotice").value,
  };
  const result = await attempt(api("POST", "announce", body));
  if (!result) {
    return;
  }

  const reached = [];
  if (result.chat_recipients !== null) {
    reached.push(`${result.chat_recipients} via chat`);
  }
  if (result.world_recipients !== null) {
    reached.push(`${result.world_recipients} on screen`);
  }
  toast("Announcement sent: " + reached.join(", "));
  byId("announceText").value = "";

  state.announceHistory.unshift({
    text,
    at: new Date(),
    reached: reached.join(", "),
  });
  const list = byId("announceHistory");
  clear(list);
  for (const entry of state.announceHistory.slice(0, 50)) {
    list.append(
      el(
        "li",
        {},
        el("span", {
          className: "muted",
          text: entry.at.toLocaleTimeString() + " - ",
        }),
        entry.text,
        el("span", {
          className: "muted small",
          text: "  (" + entry.reached + ")",
        }),
      ),
    );
  }
}

// ------------------------------------------------------------------------------------------
// Mail delivery and automatic server messages

function integerInput(input, label, minimum, maximum = Number.MAX_SAFE_INTEGER) {
  const value = Number(input.value);
  if (!input.value.trim() || !Number.isSafeInteger(value) || value < minimum || value > maximum) {
    throw new Error(`${label} must be a whole number from ${minimum} to ${maximum}.`);
  }
  return value;
}

function boundedText(text, label, maximum, allowNewlines = false, required = true) {
  if ((required && !text.trim()) || new TextEncoder().encode(text).length > maximum) {
    throw new Error(
      `${label} ${required ? "is required and " : ""}must fit within ${maximum} bytes.`,
    );
  }
  const controlCharacters = allowNewlines
    ? /[\u0000-\u0008\u000b-\u001f\u007f]/u
    : /[\u0000-\u001f\u007f]/u;
  if (controlCharacters.test(text)) {
    throw new Error(`${label} contains unsupported control characters.`);
  }
  return text;
}

function addMailAttachment() {
  const rows = byId("mailAttachmentRows");
  if (rows.children.length >= 32) {
    toast("One mail can contain up to 32 attachments.");
    return;
  }
  const kind = el(
    "select",
    { "aria-label": "Attachment reward" },
    el("option", { value: "item", text: "Item" }),
    el("option", { value: "gold", text: "Gold" }),
    el("option", { value: "token", text: "Hersander token" }),
  );
  const item = el("input", {
    type: "number",
    min: "1",
    max: "2147483647",
    step: "1",
    required: true,
    "aria-label": "Item ID",
  });
  const quantity = el("input", {
    type: "number",
    min: "1",
    max: "2147483647",
    step: "1",
    value: "1",
    required: true,
    "aria-label": "Attachment quantity",
  });
  const grade = el(
    "select",
    { "aria-label": "Equipment grade" },
    el("option", { value: "", text: "Catalog default" }),
  );
  for (let value = 0; value <= 5; value += 1) {
    grade.append(el("option", { value: String(value), text: "Grade " + value }));
  }
  const row = el(
    "tr",
    {},
    el("td", {}, kind),
    el("td", {}, item),
    el("td", {}, quantity),
    el("td", {}, grade),
  );
  row.mailFields = { kind, item, quantity, grade };
  row.append(
    el(
      "td",
      {},
      el("button", {
        type: "button",
        className: "button",
        text: "Remove",
        onClick: () => row.remove(),
      }),
    ),
  );
  kind.addEventListener("change", () => {
    const isItem = kind.value === "item";
    item.disabled = !isItem;
    item.required = isItem;
    grade.disabled = !isItem;
    if (!isItem) {
      grade.value = "";
    }
  });
  rows.append(row);
}

function mailPayload() {
  const retentionDays = integerInput(byId("mailRetention"), "Mail expiry", 1, 365);
  const body = {
    account_id: integerInput(byId("mailAccount"), "Recipient account ID", 1),
    character_id: byId("mailCharacter").value.trim()
      ? integerInput(byId("mailCharacter"), "Character ID", 1)
      : 0,
    world_id: integerInput(byId("mailWorld"), "Realm ID", 1, 2147483647),
    type: 1,
    title: boundedText(byId("mailTitle").value.trim(), "Mail title", 256),
    text: boundedText(byId("mailText").value, "Mail message", 12000, true, false),
    retention_seconds: retentionDays * 86400,
    attachments: [],
  };
  if (byId("mailAttachmentRetention").value.trim()) {
    body.attachment_retention_seconds =
      integerInput(byId("mailAttachmentRetention"), "Attachment expiry", 1, retentionDays) * 86400;
  }
  for (const row of byId("mailAttachmentRows").children) {
    const { kind, item, quantity, grade } = row.mailFields;
    let itemOrCurrencyId;
    if (kind.value === "item") {
      itemOrCurrencyId = integerInput(item, "Item ID", 1, 2147483647);
    } else {
      itemOrCurrencyId = kind.value === "gold" ? 0 : 1;
    }
    const attachment = {
      kind: kind.value === "item" ? "item" : "money",
      cms_id: itemOrCurrencyId,
      count: integerInput(quantity, "Attachment quantity", 1, 2147483647),
    };
    if (kind.value === "item" && grade.value !== "") {
      attachment.grade = Number(grade.value);
    }
    body.attachments.push(attachment);
  }
  return body;
}

function newMail() {
  if (state.mailSending) {
    return;
  }
  byId("mailForm").reset();
  if (state.overview) {
    byId("mailWorld").value = state.overview.realm.world_id;
  }
  clear(byId("mailAttachmentRows"));
  state.mailDelivery = null;
  byId("mailResult").textContent =
    "Players receive the mail in their mailbox and claim its attachments in game.";
  byId("mailSend").textContent = "Deliver mail";
}

async function deliverMail(event) {
  event.preventDefault();
  if (state.mailSending) {
    return;
  }
  let body;
  try {
    body = mailPayload();
    const signature = JSON.stringify({ server: state.connectedServer, body });
    if (!state.mailDelivery || state.mailDelivery.signature !== signature) {
      state.mailDelivery = {
        signature,
        key: crypto.randomUUID(),
      };
    }
    body.delivery_key = state.mailDelivery.key;
    if (new TextEncoder().encode(JSON.stringify(body)).length > 16384) {
      throw new Error("Mail exceeds the request size limit. Shorten its message or attachments.");
    }
  } catch (failure) {
    showError(failure);
    return;
  }
  state.mailSending = true;
  byId("mailSend").disabled = true;
  byId("mailNew").disabled = true;
  try {
    const result = await api("POST", "mail", body);
    byId("mailResult").textContent =
      `Mail #${result.mail_id} delivered. Reference: ${body.delivery_key}. Change its content or choose New mail to begin another delivery.`;
    byId("mailSend").textContent = "Check delivery again";
    toast("Mail delivered to the recipient mailbox.");
  } catch (failure) {
    // A timed-out host request may already have committed the delivery. Reuse its key while
    // the payload is unchanged so retrying cannot create a second reward.
    byId("mailResult").textContent =
      `Delivery was not confirmed. Retry with the same content to check it safely. Reference: ${body.delivery_key}.`;
    byId("mailSend").textContent = "Retry delivery";
    showError(failure);
  } finally {
    state.mailSending = false;
    byId("mailSend").disabled = false;
    byId("mailNew").disabled = false;
  }
}

function addRecurringMessage(message = { type: "chat", text: "" }) {
  const rows = byId("messageRecurringRows");
  if (rows.children.length >= 100) {
    toast("Up to 100 recurring messages are supported.");
    return;
  }
  const channel = el(
    "select",
    { "aria-label": "Message channel" },
    el("option", { value: "chat", text: "World chat" }),
    el("option", { value: "announcement", text: "Announcement banner" }),
  );
  channel.value = message.type;
  const text = el("input", {
    value: message.text,
    maxLength: 1024,
    placeholder: "Server message",
    "aria-label": "Recurring message text",
    required: true,
  });
  const row = el("div", { className: "inline-form" }, channel, text);
  row.messageFields = { channel, text };
  row.append(
    el("button", {
      type: "button",
      className: "button",
      text: "Remove",
      onClick: () => row.remove(),
    }),
  );
  rows.append(row);
}

async function loadMessageSettings() {
  state.messageSettingsLoaded = false;
  byId("messageConfigSave").disabled = true;
  const configuration = await attempt(api("GET", "server-messages"));
  if (!configuration) {
    byId("messageConfigStatus").textContent = "Automatic message settings could not be loaded.";
    return;
  }
  byId("messageSender").value = configuration.sender_name;
  byId("messageInterval").value = configuration.interval_seconds / 60;
  byId("messageRecurringEnabled").checked = configuration.enabled;
  byId("messageWelcomeEnabled").checked = configuration.welcome.enabled;
  byId("messageWelcome").value = configuration.welcome.messages.join("\n");
  clear(byId("messageRecurringRows"));
  for (const message of configuration.messages) {
    addRecurringMessage(message);
  }
  state.messageSettingsLoaded = true;
  byId("messageConfigSave").disabled = false;
  byId("messageConfigStatus").textContent =
    "Settings loaded. Welcome and recurring delivery have independent enable switches.";
}

async function saveMessageSettings(event) {
  event.preventDefault();
  if (!state.messageSettingsLoaded) {
    return;
  }
  let body;
  try {
    const seconds = Number(byId("messageInterval").value) * 60;
    const roundedSeconds = Math.round(seconds);
    if (
      !Number.isFinite(seconds) ||
      Math.abs(seconds - roundedSeconds) > 0.000001 ||
      roundedSeconds < 1 ||
      roundedSeconds > 604800
    ) {
      throw new Error("The interval must be between one second and seven days, in whole seconds.");
    }
    const welcome = byId("messageWelcome")
      .value.split(/\r?\n/u)
      .map((line) => line.trim())
      .filter(Boolean);
    if (welcome.length > 8) {
      throw new Error("Use at most eight welcome lines.");
    }
    body = {
      enabled: byId("messageRecurringEnabled").checked,
      sender_name: boundedText(byId("messageSender").value.trim(), "Server sender", 32),
      interval_seconds: roundedSeconds,
      messages: [],
      welcome: {
        enabled: byId("messageWelcomeEnabled").checked,
        messages: welcome.map((line) => boundedText(line, "Welcome line", 1024)),
      },
    };
    for (const row of byId("messageRecurringRows").children) {
      const { channel, text } = row.messageFields;
      body.messages.push({
        type: channel.value,
        text: boundedText(text.value.trim(), "Recurring message", 1024),
      });
    }
    if (body.enabled && body.messages.length === 0) {
      throw new Error("Add a recurring message before enabling recurring delivery.");
    }
    if (body.welcome.enabled && body.welcome.messages.length === 0) {
      throw new Error("Enter welcome text before enabling welcome delivery.");
    }
    if (new TextEncoder().encode(JSON.stringify(body)).length > 16384) {
      throw new Error("Automatic messages exceed the request size limit. Shorten the text.");
    }
  } catch (failure) {
    showError(failure);
    return;
  }
  byId("messageConfigSave").disabled = true;
  try {
    const result = await api("POST", "server-messages", body);
    byId("messageConfigStatus").textContent = result.settings_saved
      ? "Automatic message settings saved and active."
      : "Automatic messages updated for this run. Settings could not be saved for restart.";
    toast(
      result.settings_saved ? "Automatic messages saved." : "Messages updated for this run only.",
    );
  } catch (failure) {
    showError(failure);
  } finally {
    byId("messageConfigSave").disabled = false;
  }
}

// ------------------------------------------------------------------------------------------
// Live logs

function logLineVisible(entry) {
  const filter = byId("logSearch").value.trim().toLowerCase();
  return !filter || entry.message.toLowerCase().includes(filter);
}

function logLineNode(entry) {
  if (entry.gap) {
    return el("div", { className: "log-line log-gap", text: entry.message });
  }
  return el(
    "div",
    { className: "log-line" },
    el("span", { className: "log-time", text: formatTimeOfDay(entry.unix_ms) }),
    el("span", {
      className: "log-level log-" + entry.level,
      text: entry.level,
    }),
    el("span", { text: entry.message }),
  );
}

function appendLogEntries(entries) {
  const view = byId("logView");
  const follow = byId("logFollow").checked;

  state.logs.entries.push(...entries);
  const overflow = state.logs.entries.length - MAXIMUM_LOG_LINES;
  if (overflow > 0) {
    state.logs.entries.splice(0, overflow);
    // Rendering from scratch is simpler than tracking which DOM rows were evicted.
    renderAllLogs();
  } else {
    for (const entry of entries) {
      if (logLineVisible(entry)) {
        view.append(logLineNode(entry));
      }
    }
  }
  if (follow) {
    view.scrollTop = view.scrollHeight;
  }
}

function renderAllLogs() {
  const view = byId("logView");
  clear(view);
  for (const entry of state.logs.entries) {
    if (logLineVisible(entry)) {
      view.append(logLineNode(entry));
    }
  }
  if (byId("logFollow").checked) {
    view.scrollTop = view.scrollHeight;
  }
}

function resetLogs() {
  state.logs.entries = [];
  state.logs.after = 0;
  clear(byId("logView"));
}

async function pollLogs() {
  if (!state.connected || state.logs.paused || state.logs.inFlight || state.page !== "logs") {
    return;
  }
  state.logs.inFlight = true;
  try {
    const level = byId("logFilterLevel").value;
    const page = await api(
      "GET",
      `logs?after=${state.logs.after}&limit=${LOG_PAGE_SIZE}&level=${level}`,
    );
    const entries = page.entries.slice();
    if (page.gap && state.logs.after !== 0) {
      entries.unshift({
        gap: true,
        message: "... older lines were dropped or the server restarted ...",
      });
    }
    state.logs.after = page.next_after;
    if (entries.length > 0) {
      appendLogEntries(entries);
    }
    byId("logStatus").textContent =
      `${state.logs.entries.length} lines in view; latest #${page.latest_sequence}`;
  } catch (failure) {
    byId("logStatus").textContent = "Log poll failed: " + (failure.message || "unknown error");
  } finally {
    state.logs.inFlight = false;
  }
}

async function refreshServerLogLevel() {
  const overview = await refreshOverview();
  if (overview) {
    byId("serverLogLevel").value =
      overview.server.log_level === "warn" ? "warning" : overview.server.log_level;
  }
}

async function setServerLogLevel() {
  const level = byId("serverLogLevel").value;
  await attempt(api("POST", "logs/level", { level }), "Server log level set to " + level);
}

// ------------------------------------------------------------------------------------------
// World chat

function chatMessageVisible(message) {
  const filter = byId("chatSearch").value.trim().toLowerCase();
  if (!filter) {
    return true;
  }
  return (
    message.sender_name.toLowerCase().includes(filter) ||
    message.message.toLowerCase().includes(filter)
  );
}

function chatLineNode(message) {
  if (message.gap) {
    return el("div", { className: "chat-line log-gap", text: message.text });
  }

  const line = el(
    "div",
    {
      className: "chat-line" + (message.operator_notice ? " chat-operator" : ""),
    },
    el("span", {
      className: "chat-time",
      text: formatTimeOfDay(message.unix_ms),
    }),
    el("span", { className: "chat-sender", text: message.sender_name + ":" }),
    el("span", { className: "chat-text", text: message.message }),
  );
  if (message.character_id) {
    line.title = "Character #" + message.character_id + " (click to open)";
    line.style.cursor = "pointer";
    line.addEventListener("click", () => openCharacter(message.character_id));
  }
  return line;
}

function renderAllChat() {
  const view = byId("chatView");
  clear(view);
  const visible = state.chat.messages.filter(
    (message) => message.gap || chatMessageVisible(message),
  );
  if (visible.length === 0) {
    view.append(
      el("div", {
        className: "chat-empty",
        text: "No World chat messages yet",
      }),
    );
  }
  for (const message of visible) {
    view.append(chatLineNode(message));
  }
  if (byId("chatFollow").checked) {
    view.scrollTop = view.scrollHeight;
  }
}

function appendChatMessages(messages) {
  state.chat.messages.push(...messages);
  const overflow = state.chat.messages.length - MAXIMUM_CHAT_LINES;
  if (overflow > 0) {
    state.chat.messages.splice(0, overflow);
  }
  // Re-rendering keeps the empty-state text and evictions simple; the view is bounded.
  renderAllChat();
}

function setChatUnread(count) {
  state.chat.unread = count;
  const badgeNode = byId("chatUnread");
  badgeNode.textContent = count > 99 ? "99+" : String(count);
  badgeNode.classList.toggle("hidden", count === 0);
}

function markChatRead() {
  setChatUnread(0);
  renderAllChat();
}

async function pollChat() {
  const chat = state.chat;
  if (state.page === "realm" && state.realmDirty) {
    return;
  }
  if (!state.connected || chat.paused || chat.inFlight || !chat.available) {
    return;
  }
  chat.inFlight = true;
  try {
    const page = await api("GET", `chat?after=${chat.after}&limit=${CHAT_PAGE_SIZE}`);
    const messages = page.messages.slice();
    if (page.gap && chat.after !== 0) {
      messages.unshift({
        gap: true,
        text: "... older messages were dropped or the server restarted ...",
      });
    }
    chat.after = page.next_after;

    const playerMessages = page.messages.filter((message) => !message.operator_notice).length;
    if (messages.length > 0) {
      appendChatMessages(messages);
      if (state.page !== "chat") {
        setChatUnread(chat.unread + playerMessages);
      }
    }
    byId("chatStatus").textContent =
      `${chat.messages.length} message(s) in view; updated ${new Date().toLocaleTimeString()}. ` +
      "The server keeps the latest 1,000 in memory only.";
  } catch (failure) {
    if (failure.error === "service_unavailable") {
      // A server without front chat will not gain it until restarted with it.
      chat.available = false;
      byId("chatStatus").textContent = failure.message;
    } else {
      byId("chatStatus").textContent = "Chat poll failed: " + (failure.message || "unknown error");
    }
  } finally {
    chat.inFlight = false;
  }
}

function resetChat() {
  state.chat.after = 0;
  state.chat.messages = [];
  state.chat.available = true;
  setChatUnread(0);
  renderAllChat();
}

async function sendChat(event) {
  event.preventDefault();
  const input = byId("chatMessage");
  const text = input.value.trim();
  if (!text) {
    return;
  }

  const body = {
    text,
    chat: true,
    chat_sender: byId("chatSender").value.trim() || "[Server]",
    system_notice: "none",
  };
  const result = await attempt(api("POST", "announce", body));
  if (result) {
    input.value = "";
    pollChat();
  }
}

// ------------------------------------------------------------------------------------------
// Server

async function shutdownServer() {
  const values = await formDialog({
    title: "Shut down server",
    description:
      "Connected players are disconnected after their positions are saved. Type shutdown to confirm.",
    fields: [{ id: "confirm", label: "Confirmation", required: true }],
    confirmLabel: "Shut down",
    danger: true,
  });
  if (!values) {
    return;
  }
  const result = await attempt(api("POST", "server/shutdown", { confirm: values.confirm.trim() }));
  if (result) {
    toast("The server is shutting down");
    setTimeout(() => setConnected(false, "Server stopped"), 1000);
  }
}

// ------------------------------------------------------------------------------------------
// Wiring

function bindEvents() {
  document.querySelectorAll(".nav-item").forEach((button) => {
    button.addEventListener("click", () => navigate(button.dataset.page));
  });
  document.querySelectorAll("[data-window]").forEach((button) => {
    button.addEventListener("click", () => send("window_" + button.dataset.window));
  });
  byId("windowDrag").addEventListener("mousedown", (event) => {
    if (event.button === 0) {
      send("window_drag");
    }
  });
  byId("windowDrag").addEventListener("dblclick", () => send("window_maximize"));

  byId("dashboardRefresh").addEventListener("click", refreshOverview);
  byId("dashMaintenance").addEventListener("change", (event) =>
    quickRealmToggle("maintenance", event.target),
  );
  byId("dashCreation").addEventListener("change", (event) =>
    quickRealmToggle("char_creation_restricted", event.target),
  );

  byId("realmForm").addEventListener("input", markRealmEdited);
  byId("realmForm").addEventListener("change", markRealmEdited);
  byId("realmForm").addEventListener("submit", saveRealm);

  byId("accountRefresh").addEventListener("click", loadAccounts);
  byId("accountCreate").addEventListener("click", createAccount);
  byId("accountSearch").addEventListener("keydown", (event) => {
    if (event.key === "Enter") {
      loadAccounts();
    }
  });

  byId("characterRefresh").addEventListener("click", loadCharacters);
  byId("characterSearch").addEventListener("keydown", (event) => {
    if (event.key === "Enter") {
      loadCharacters();
    }
  });

  byId("playersRefresh").addEventListener("click", loadPlayers);

  document.querySelectorAll("#ratePresets [data-rate]").forEach((button) => {
    button.addEventListener("click", () => setBaseRate(Number(button.dataset.rate)));
  });
  byId("rateBase").addEventListener("input", markRateEdited);
  byId("rateForm").addEventListener("submit", (event) => {
    event.preventDefault();
    setBaseRate(Number(byId("rateBase").value));
  });
  byId("eventCreate").addEventListener("click", createEvent);

  byId("announceForm").addEventListener("submit", sendAnnouncement);
  byId("mailForm").addEventListener("submit", deliverMail);
  byId("mailAddAttachment").addEventListener("click", addMailAttachment);
  byId("mailNew").addEventListener("click", newMail);
  byId("messageConfigForm").addEventListener("submit", saveMessageSettings);
  byId("messageConfigRefresh").addEventListener("click", loadMessageSettings);
  byId("messageAddRecurring").addEventListener("click", () => addRecurringMessage());

  byId("logPause").addEventListener("click", () => {
    state.logs.paused = !state.logs.paused;
    byId("logPause").textContent = state.logs.paused ? "Resume" : "Pause";
  });
  byId("logClear").addEventListener("click", () => {
    state.logs.entries = [];
    clear(byId("logView"));
  });
  byId("logSearch").addEventListener("input", renderAllLogs);
  byId("logFilterLevel").addEventListener("change", resetLogs);
  byId("serverLogLevel").addEventListener("change", setServerLogLevel);

  byId("adminServerChoice").addEventListener("change", () => connect());
  byId("connectForm").addEventListener("submit", (event) => {
    event.preventDefault();
    connect(false);
  });
  byId("connectDefaults").addEventListener("click", () => {
    byId("connectUrl").value = byId("connectDefaults").dataset.url || "";
    byId("connectTokenFile").value = byId("connectDefaults").dataset.tokenFile || "";
  });
  byId("shutdownButton").addEventListener("click", shutdownServer);
  byId("drawerClose").addEventListener("click", closeDrawer);
  document.addEventListener("keydown", (event) => {
    if (event.key === "Escape" && !byId("modal").open) {
      closeDrawer();
    }
  });

  window.chrome.webview.addEventListener("message", (event) => handleHostMessage(event.data));
}

function bindChatEvents() {
  byId("chatForm").addEventListener("submit", sendChat);
  byId("chatSearch").addEventListener("input", renderAllChat);
  byId("chatPause").addEventListener("click", () => {
    state.chat.paused = !state.chat.paused;
    byId("chatPause").textContent = state.chat.paused ? "Resume" : "Pause";
  });
  byId("chatClear").addEventListener("click", () => {
    state.chat.messages = [];
    renderAllChat();
  });
}

function startPolling() {
  setInterval(pollChat, POLL_INTERVAL_MS.chat);
  setInterval(() => {
    if (state.page === "dashboard") {
      refreshOverview();
    }
  }, POLL_INTERVAL_MS.dashboard);
  setInterval(() => {
    if (state.page === "players" && byId("playersAuto").checked) {
      loadPlayers();
    }
  }, POLL_INTERVAL_MS.players);
  setInterval(pollLogs, POLL_INTERVAL_MS.logs);
  setInterval(tickClock, POLL_INTERVAL_MS.clock);
}

bindEvents();
bindChatEvents();
startPolling();
send("ui_ready");
