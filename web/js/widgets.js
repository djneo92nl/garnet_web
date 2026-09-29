// Custom widgets a group can ask for with GsGroup.widget - flows that don't
// fit a plain field: WiFi scan/join/forget, password change, backup/restore.

// ---- <gw-wifi> -------------------------------------------------------------

function gwBars(rssi) {
  const level = rssi > -55 ? 4 : rssi > -67 ? 3 : rssi > -75 ? 2 : 1;
  return html`<span class="gw-bars" title="${rssi} dBm"
    >${[5, 8, 11, 14].map((h, i) => html`<i class=${i < level ? "on" : ""} style="height:${h}px"></i>`)}</span
  >`;
}

class GwWifi extends GwElement {
  static properties = { st: { state: true } };

  connectedCallback() {
    super.connectedCallback();
    this.refresh().then(() => this.scan());
  }

  disconnectedCallback() {
    super.disconnectedCallback();
    clearTimeout(this.timer);
  }

  async refresh() {
    try {
      this.st = await gwApi.get("/api/wifi");
    } catch (_) {
      return;
    }
    clearTimeout(this.timer);
    if (this.st.scan.state === "running") this.timer = setTimeout(() => this.refresh(), 1500);
  }

  async scan() {
    try {
      await gwApi.post("/api/wifi/scan");
    } catch (_) {
      return;
    }
    if (this.st) this.st = { ...this.st, scan: { ...this.st.scan, state: "running" } };
    clearTimeout(this.timer);
    this.timer = setTimeout(() => this.refresh(), 1500);
  }

  async join(ssid, secure) {
    const res = await gwDialog({
      title: ssid ? `Join “${ssid}”` : "Other Network",
      text: "Saves the network and restarts to join it.",
      ok: "Join",
      inputs: [
        ...(ssid ? [] : [{ name: "ssid", label: "Network name" }]),
        ...(secure || !ssid ? [{ name: "password", type: "password", label: "Password", autocomplete: "new-password" }] : []),
      ],
      submit: async (v) => {
        try {
          await gwApi.post("/api/wifi/add", { ssid: ssid || v.ssid, password: v.password || "" });
        } catch (e) {
          return e.message;
        }
      },
    });
    if (res) gwRebooting();
  }

  async forget(ssid) {
    const ok = await gwDialog({
      title: `Forget “${ssid}”?`,
      text: "The device will no longer join this network automatically.",
      ok: "Forget",
      danger: true,
    });
    if (!ok) return;
    try {
      await gwApi.post("/api/wifi/forget", { ssid });
    } catch (_) {}
    this.refresh();
  }

  render() {
    const st = this.st;
    if (!st) return nothing;
    const scanning = st.scan.state === "running";
    const others = st.scan.nets.filter((n) => !(st.connected && n.ssid === st.ssid));
    return html`
      ${st.portal ? html`<p class="gw-note">Setup AP${st.apSsid ? ` \u201c${st.apSsid}\u201d` : ""} active. Pick a network to join.</p>` : nothing}
      ${st.mode === "ethernet" ? html`<p class="gw-note">Wi-Fi is off while Ethernet is connected.</p>` : nothing}
      ${st.saved.length
        ? html`<div class="gw-card-title">Saved networks</div>
            <div class="gw-listbox">
              ${st.saved.map(
                (s) => html`<div class="gw-row list">
                  <span class="gw-mark">${st.connected && s === st.ssid ? "\u2713" : ""}</span>
                  <span class="gw-label">${s}</span>
                  <button class="gw-btn" @click=${() => this.forget(s)}>Forget</button>
                </div>`
              )}
            </div>`
        : nothing}
      <div class="gw-card-title">
        Networks
        ${scanning
          ? html`<span class="gw-busy">scanning\u2026</span>`
          : html`<button class="gw-link gw-busy" @click=${() => this.scan()}>rescan</button>`}
      </div>
      <div class="gw-listbox">
        ${others.map(
          (n) => html`<div class="gw-row list clickable" @click=${() => this.join(n.ssid, n.secure)}>
            <span class="gw-mark"></span>
            <span class="gw-label">${n.ssid}</span>
            ${n.secure ? gwLockIcon : nothing} ${gwBars(n.rssi)}
          </div>`
        )}
        ${!others.length && !scanning ? html`<div class="gw-row list"><span class="gw-mark"></span><span class="gw-label">None found</span></div>` : nothing}
        <div class="gw-row list clickable" @click=${() => this.join(null, true)}>
          <span class="gw-mark"></span><span class="gw-label">Other\u2026</span>
        </div>
      </div>
    `;
  }
}
customElements.define("gw-wifi", GwWifi);

// ---- <gw-files> ---------------------------------------------------------------
// File manager for GARNET_WEB_FILES. Palm-style interaction: tap a row to
// select it (tap a folder again to open it), then act on the selection with
// the buttons under the list.

function gwJoin(dir, name) {
  return (dir === "/" ? "" : dir) + "/" + name;
}

function gwFmtSize(n) {
  if (n === undefined) return "";
  if (n >= 1073741824) return (n / 1073741824).toFixed(2) + " GB";
  if (n >= 1048576) return (n / 1048576).toFixed(1) + " MB";
  if (n >= 1024) return (n / 1024).toFixed(1) + " KB";
  return n + " B";
}

class GwFiles extends GwElement {
  static properties = {
    stores: { state: true },
    fs: { state: true },
    path: { state: true },
    entries: { state: true },
    sel: { state: true },
    note: { state: true },
    busy: { state: true },
  };

  constructor() {
    super();
    this.path = "/";
    this.entries = null;
    this.sel = null;
  }

  connectedCallback() {
    super.connectedCallback();
    this.loadStores();
  }

  async loadStores() {
    try {
      this.stores = await gwApi.get("/api/fs");
    } catch (e) {
      this.note = { error: true, text: e.message };
      return;
    }
    if (this.stores.length && !this.stores.find((s) => s.id === this.fs)) this.open(this.stores[0].id, "/");
  }

  q(extra) {
    const p = new URLSearchParams({ fs: this.fs, ...extra });
    return p.toString();
  }

  async open(fs, path) {
    this.fs = fs;
    this.path = path;
    this.sel = null;
    this.entries = null;
    try {
      const res = await gwApi.get(`/api/fs/list?${this.q({ path })}`);
      res.entries.sort((a, b) => (b.dir - a.dir) || a.name.localeCompare(b.name));
      this.entries = res.entries;
    } catch (e) {
      this.entries = [];
      this.note = { error: true, text: e.message };
    }
  }

  refresh() {
    this.open(this.fs, this.path);
    this.loadStores();
  }

  tap(ent) {
    if (ent.dir && this.sel === ent.name) {
      this.note = null;
      this.open(this.fs, gwJoin(this.path, ent.name));
    } else {
      this.sel = ent.name;
    }
  }

  up() {
    const i = this.path.lastIndexOf("/");
    this.open(this.fs, i <= 0 ? "/" : this.path.slice(0, i));
  }

  selected() {
    return this.entries && this.entries.find((e) => e.name === this.sel);
  }

  download() {
    const ent = this.selected();
    if (!ent || ent.dir) return;
    const a = document.createElement("a");
    a.href = `/api/fs/get?${this.q({ path: gwJoin(this.path, ent.name), dl: "1" })}`;
    a.download = ent.name;
    a.click();
  }

  view() {
    const ent = this.selected();
    if (ent && !ent.dir) window.open(`/api/fs/get?${this.q({ path: gwJoin(this.path, ent.name) })}`, "_blank");
  }

  async op(name, body, okText) {
    try {
      await gwApi.post(`/api/fs/${name}`, { fs: this.fs, ...body });
      this.note = okText ? { text: okText } : null;
      this.refresh();
      return null;
    } catch (e) {
      return e.message;
    }
  }

  async newFolder() {
    await gwDialog({
      title: "New Folder",
      ok: "Create",
      inputs: [{ name: "name", label: "Name" }],
      submit: (v) => (v.name ? this.op("mkdir", { path: gwJoin(this.path, v.name) }) : "Enter a name"),
    });
  }

  async rename() {
    const ent = this.selected();
    if (!ent) return;
    const from = gwJoin(this.path, ent.name);
    await gwDialog({
      title: "Rename / Move",
      text: "A full path moves it to another folder.",
      ok: "Rename",
      inputs: [{ name: "to", label: "New name or path", value: ent.name }],
      submit: (v) => {
        if (!v.to) return "Enter a name";
        const to = v.to.startsWith("/") ? v.to : gwJoin(this.path, v.to);
        return to === from ? null : this.op("rename", { from, to });
      },
    });
  }

  async remove() {
    const ent = this.selected();
    if (!ent) return;
    await gwDialog({
      title: ent.dir ? "Delete Folder" : "Delete File",
      text: `Delete \u201c${ent.name}\u201d?${ent.dir ? " The folder must be empty." : ""}`,
      ok: "Delete",
      danger: true,
      submit: () => this.op("delete", { path: gwJoin(this.path, ent.name) }),
    });
  }

  pickUpload() {
    this.querySelector("#gw-upload").click();
  }

  // Sequential XHR uploads (progress per file); the device streams each
  // body straight to "<name>.part" and renames when complete.
  async upload(e) {
    const files = [...e.target.files];
    e.target.value = "";
    if (!files.length) return;
    this.busy = true;
    let done = 0;
    for (const file of files) {
      const err = await new Promise((resolve) => {
        const xhr = new XMLHttpRequest();
        xhr.open("POST", `/api/fs/put?${this.q({ path: gwJoin(this.path, file.name) })}`);
        xhr.setRequestHeader("X-GW", "1");
        xhr.setRequestHeader("Content-Type", "application/octet-stream");
        xhr.upload.onprogress = (ev) => {
          if (ev.lengthComputable) {
            const pct = Math.round((ev.loaded * 100) / ev.total);
            this.note = { text: `Uploading ${done + 1}/${files.length} ${file.name} ${pct}%` };
          }
        };
        xhr.onload = () => {
          let res = null;
          try {
            res = JSON.parse(xhr.responseText);
          } catch (_) {}
          resolve(xhr.status === 200 ? null : (res && res.error) || `Failed (${xhr.status})`);
        };
        xhr.onerror = () => resolve("Upload interrupted");
        xhr.send(file);
      });
      if (err) {
        this.note = { error: true, text: `${file.name}: ${err}` };
        this.busy = false;
        this.refresh();
        return;
      }
      done++;
    }
    this.busy = false;
    this.note = { text: `Uploaded ${done} file${done === 1 ? "" : "s"}` };
    this.refresh();
  }

  render() {
    if (!this.stores) return html`${this.note ? html`<p class="gw-note error">${this.note.text}</p>` : html`<div class="gw-empty">Loading\u2026</div>`}`;
    if (!this.stores.length) return html`<p class="gw-note">No storage mounted.</p>`;
    const store = this.stores.find((s) => s.id === this.fs) || this.stores[0];
    const ent = this.selected();
    const crumbs = this.path === "/" ? [] : this.path.slice(1).split("/");
    return html`
      <div class="gw-row">
        <span class="gw-label"><span class="gw-lt">Storage</span></span>
        ${this.stores.length > 1
          ? html`<select aria-label="Storage" @change=${(e) => this.open(e.target.value, "/")}>
              ${this.stores.map((s) => html`<option value=${s.id} ?selected=${s.id === store.id}>${s.title}</option>`)}
            </select>`
          : html`<span class="gw-value">${store.title}</span>`}
      </div>
      <div class="gw-row">
        <span class="gw-label"><span class="gw-lt">Used</span></span>
        <span class="gw-value">${gwFmtSize(store.used)} of ${gwFmtSize(store.total)}</span>
      </div>
      <div class="gw-card-title">
        <button class="gw-link gw-busy" @click=${() => this.open(this.fs, "/")}>${store.title}</button>
        ${crumbs.map(
          (c, i) => html`\u203a <button class="gw-link gw-busy"
            @click=${() => this.open(this.fs, "/" + crumbs.slice(0, i + 1).join("/"))}>${c}</button>`
        )}
      </div>
      <div class="gw-listbox gw-files">
        ${this.path !== "/"
          ? html`<div class="gw-row list clickable" @click=${this.up}>
              <span class="gw-mark">${gwIcon("folder")}</span><span class="gw-label">..</span>
            </div>`
          : nothing}
        ${this.entries === null
          ? html`<div class="gw-row list"><span class="gw-label">Loading\u2026</span></div>`
          : this.entries.length === 0
          ? html`<div class="gw-row list"><span class="gw-label">Empty</span></div>`
          : this.entries.map(
              (en) => html`<div class="gw-row list clickable ${this.sel === en.name ? "selected" : ""}"
                @click=${() => this.tap(en)} @dblclick=${() => en.dir || this.view()}>
                <span class="gw-mark">${gwIcon(en.dir ? "folder" : "file")}</span>
                <span class="gw-label">${en.name}</span>
                <span class="gw-meta">${en.dir ? "" : gwFmtSize(en.size)}</span>
                <span class="gw-meta gw-date">${en.time ? new Date(en.time * 1000).toLocaleDateString() : ""}</span>
              </div>`
            )}
      </div>
      ${this.note ? html`<p class="gw-note ${this.note.error ? "error" : ""}">${this.note.text}</p>` : nothing}
      <div class="gw-buttons">
        <button class="gw-btn" ?disabled=${this.busy} @click=${this.pickUpload}>Upload\u2026</button>
        <button class="gw-btn" ?disabled=${this.busy} @click=${this.newFolder}>New Folder\u2026</button>
        <button class="gw-btn" ?disabled=${!ent || ent.dir} @click=${this.download}>Download</button>
        <button class="gw-btn" ?disabled=${!ent} @click=${this.rename}>Rename\u2026</button>
        <button class="gw-btn danger" ?disabled=${!ent} @click=${this.remove}>Delete\u2026</button>
      </div>
      <input id="gw-upload" type="file" multiple hidden @change=${this.upload} />
    `;
  }
}
customElements.define("gw-files", GwFiles);

// ---- Polling helper ---------------------------------------------------------------
// Tool widgets poll while their tab is visible and stop when hidden or
// removed - the same rule as the Info rows.

class GwPoller extends GwElement {
  pollMs = 1000;

  connectedCallback() {
    super.connectedCallback();
    this.onVis = () => this.schedule(0);
    document.addEventListener("visibilitychange", this.onVis);
    this.schedule(0, true);
  }

  disconnectedCallback() {
    super.disconnectedCallback();
    document.removeEventListener("visibilitychange", this.onVis);
    clearTimeout(this.timer);
  }

  // `force`: the first fetch on open runs even in a hidden tab, so the
  // widget never opens empty; only the repeating poll waits for visibility.
  schedule(ms, force = false) {
    clearTimeout(this.timer);
    if (!force && document.visibilityState !== "visible") return;
    this.timer = setTimeout(async () => {
      try {
        await this.tick();
      } catch (_) {
        // transient - next tick retries
      }
      this.schedule(this.pollMs);
    }, ms);
  }
}

// Terminal box that keeps the view pinned to the bottom unless the user
// scrolled up to read.
function gwTermUpdated(host) {
  const box = host.querySelector(".gw-term");
  if (box && host.stick !== false) box.scrollTop = box.scrollHeight;
}
function gwTermScrolled(host, e) {
  const b = e.target;
  host.stick = b.scrollHeight - b.scrollTop - b.clientHeight < 24;
}

const GW_TERM_MAX = 200000; // chars kept client-side

// ---- <gw-log> -------------------------------------------------------------------------

class GwLog extends GwPoller {
  static properties = { text: { state: true }, paused: { state: true } };

  constructor() {
    super();
    this.text = "";
    this.since = 0;
  }

  async tick() {
    if (this.paused) return;
    const r = await gwApi.get(`/api/log?since=${this.since}`);
    if (r.head < this.since) this.text += "\n--- device restarted ---\n";
    let add = r.text;
    if (r.gap && this.since) add = "\n--- older lines dropped ---\n" + add;
    this.since = r.head;
    if (add) this.text = (this.text + add).slice(-GW_TERM_MAX);
  }

  download() {
    const a = document.createElement("a");
    a.href = URL.createObjectURL(new Blob([this.text], { type: "text/plain" }));
    a.download = `log-${new Date().toISOString().slice(0, 19).replace(/:/g, "")}.txt`;
    a.click();
    URL.revokeObjectURL(a.href);
  }

  updated() {
    gwTermUpdated(this);
  }

  render() {
    return html`
      <pre class="gw-term" @scroll=${(e) => gwTermScrolled(this, e)}>${this.text || "Waiting for output\u2026"}</pre>
      <div class="gw-buttons">
        <button class="gw-btn" @click=${() => (this.paused = !this.paused)}>${this.paused ? "Resume" : "Pause"}</button>
        <button class="gw-btn" @click=${() => (this.text = "")}>Clear</button>
        <button class="gw-btn" ?disabled=${!this.text} @click=${this.download}>Download</button>
      </div>`;
  }
}
customElements.define("gw-log", GwLog);

// ---- <gw-uart> ------------------------------------------------------------------------

function gwHexToBytes(hex) {
  const out = new Uint8Array(hex.length / 2);
  for (let i = 0; i < out.length; i++) out[i] = parseInt(hex.substr(i * 2, 2), 16);
  return out;
}

class GwUart extends GwPoller {
  static properties = { bytes: { state: true }, mode: { state: true }, st: { state: true }, note: { state: true } };
  pollMs = 500;

  constructor() {
    super();
    this.bytes = new Uint8Array(0);
    this.since = 0;
    this.mode = "text";
    this.eol = "lf";
  }

  async tick() {
    const r = await gwApi.get(`/api/uart?since=${this.since}`);
    this.st = r;
    if (r.head < this.since) this.bytes = new Uint8Array(0);
    this.since = r.head;
    if (!r.hex) return;
    const add = gwHexToBytes(r.hex);
    const merged = new Uint8Array(Math.min(this.bytes.length + add.length, GW_TERM_MAX));
    const keep = merged.length - add.length;
    merged.set(this.bytes.subarray(this.bytes.length - keep), 0);
    merged.set(add.subarray(Math.max(0, add.length - merged.length)), Math.max(0, keep));
    this.bytes = merged;
  }

  view() {
    if (this.mode === "hex") {
      const lines = [];
      for (let i = 0; i < this.bytes.length; i += 16) {
        const row = this.bytes.subarray(i, i + 16);
        const hex = [...row].map((b) => b.toString(16).padStart(2, "0")).join(" ");
        const asc = [...row].map((b) => (b >= 32 && b < 127 ? String.fromCharCode(b) : ".")).join("");
        lines.push(`${i.toString(16).padStart(6, "0")}  ${hex.padEnd(47)}  ${asc}`);
      }
      return lines.join("\n");
    }
    // Text: decode as UTF-8 (lenient), drop CR so CRLF lines don't double.
    return new TextDecoder("utf-8", { fatal: false }).decode(this.bytes).replace(/\r/g, "");
  }

  async send(e) {
    e.preventDefault();
    const input = this.querySelector("#gw-uart-in");
    try {
      await gwApi.post("/api/uart/send", { text: input.value, eol: this.eol });
      input.value = "";
      this.note = null;
    } catch (err) {
      this.note = { error: true, text: err.message };
    }
  }

  updated() {
    gwTermUpdated(this);
  }

  render() {
    const st = this.st;
    return html`
      ${st && !st.open ? html`<p class="gw-note">Serial is off. Set pins and enable it below.</p>` : nothing}
      <pre class="gw-term" @scroll=${(e) => gwTermScrolled(this, e)}>${this.view() || (st && st.open ? "Waiting for data\u2026" : "")}</pre>
      <form class="gw-inline" @submit=${this.send}>
        <input id="gw-uart-in" type="text" autocomplete="off" autocapitalize="off" spellcheck="false"
          placeholder=${st && st.canSend ? "Send\u2026" : "Listen only"} ?disabled=${!(st && st.canSend)} />
        <select aria-label="Line ending" @change=${(e) => (this.eol = e.target.value)}>
          <option value="lf" selected>LF</option><option value="crlf">CR LF</option>
          <option value="cr">CR</option><option value="none">None</option>
        </select>
        <button class="gw-btn" type="submit" ?disabled=${!(st && st.canSend)}>Send</button>
      </form>
      ${this.note ? html`<p class="gw-note error">${this.note.text}</p>` : nothing}
      <div class="gw-buttons">
        <button class="gw-btn" @click=${() => (this.mode = this.mode === "hex" ? "text" : "hex")}>
          ${this.mode === "hex" ? "Text view" : "Hex view"}</button>
        <button class="gw-btn" @click=${() => (this.bytes = new Uint8Array(0))}>Clear</button>
      </div>`;
  }
}
customElements.define("gw-uart", GwUart);

// ---- <gw-hw> ---------------------------------------------------------------------------
// Likely parts per I2C address - several chips share addresses, so all
// plausible ones are listed.
const GW_I2C_NAMES = {
  0x0c: "AK8963 magnetometer", 0x10: "VEML7700 light", 0x18: "LIS3DH accel / MCP9808",
  0x19: "LSM303 accel", 0x1d: "ADXL345 / MMA8451", 0x1e: "HMC5883L compass",
  0x20: "PCF8574 / MCP23017 IO", 0x21: "PCF8574 / MCP23017 IO", 0x23: "BH1750 light",
  0x24: "PN532 NFC", 0x27: "PCF8574 (LCD backpack)", 0x28: "BNO055 IMU", 0x29: "VL53L0X / TSL2591 / BNO055",
  0x2c: "AD5245 / CAP1188", 0x36: "MAX17048 fuel gauge / seesaw", 0x38: "AHT10/20 / FT6236 touch",
  0x39: "TSL2561 / APDS9960", 0x3c: "SSD1306 / SH1106 OLED", 0x3d: "SSD1306 OLED (alt)",
  0x40: "INA219 / HTU21D / SHT21 / PCA9685", 0x41: "INA219 (alt)", 0x44: "SHT3x / SHT4x",
  0x45: "SHT3x (alt)", 0x48: "ADS1115 / TMP102 / PCF8591", 0x49: "ADS1115 / TSL2561",
  0x4a: "ADS1115 / MAX44009", 0x4b: "ADS1115", 0x50: "AT24C EEPROM", 0x51: "PCF8563 RTC / EEPROM",
  0x53: "ADXL345 (alt)", 0x57: "MAX30102 / EEPROM (DS3231 board)", 0x58: "SGP30 / SGP40",
  0x5a: "MLX90614 / CCS811 / MPR121", 0x5b: "CCS811 (alt)", 0x5c: "AM2320 / BH1750 (alt)",
  0x60: "MPL3115A2 / Si5351 / ATECC", 0x61: "SCD30 CO2", 0x62: "SCD40/41 CO2", 0x68: "DS3231 / DS1307 RTC / MPU6050",
  0x69: "MPU6050 (alt) / ICM-20948", 0x6a: "LSM6DS IMU", 0x6b: "LSM6DS IMU (alt)",
  0x70: "HT16K33 / TCA9548A mux", 0x76: "BME280 / BMP280 / BME680", 0x77: "BME280 / BMP180 / BME680 (alt)",
};

class GwHw extends GwElement {
  static properties = {
    info: { state: true }, scan: { state: true }, pinRes: { state: true },
    inputs: { state: true }, busy: { state: true }, err: { state: true },
  };

  connectedCallback() {
    super.connectedCallback();
    gwApi.get("/api/hw").then((i) => (this.info = i)).catch((e) => (this.err = e.message));
  }

  pref(key, fallback) {
    try {
      const v = localStorage.getItem("gw-hw-" + key);
      return v === null ? fallback : Number(v);
    } catch (_) {
      return fallback;
    }
  }

  remember(key, v) {
    try {
      localStorage.setItem("gw-hw-" + key, String(v));
    } catch (_) {}
  }

  num(id) {
    return Number(this.querySelector(id).value);
  }

  async run(fn) {
    this.busy = true;
    this.err = null;
    try {
      await fn();
    } catch (e) {
      this.err = e.message;
    } finally {
      this.busy = false;
    }
  }

  i2c() {
    const sda = this.num("#gw-sda"), scl = this.num("#gw-scl");
    this.remember("sda", sda);
    this.remember("scl", scl);
    this.scan = null;
    return this.run(async () => (this.scan = await gwApi.post("/api/hw/i2c", { sda, scl })));
  }

  gpio(op) {
    const pin = this.num("#gw-pin");
    this.remember("pin", pin);
    return this.run(async () => (this.pinRes = { op, ...(await gwApi.post("/api/hw/gpio", { pin, op })) }));
  }

  readInputs() {
    return this.run(async () => (this.inputs = (await gwApi.post("/api/hw/inputs")).pins));
  }

  pinSummary() {
    const i = this.info;
    const pick = (f) => i.pins.filter(f).map((p) => p.n).join(", ") || "none";
    return html`
      <div class="gw-row"><span class="gw-label"><span class="gw-lt">Reserved</span><span class="gw-help">Flash / PSRAM, never touched</span></span><span class="gw-value">${pick((p) => p.reserved)}</span></div>
      <div class="gw-row"><span class="gw-label"><span class="gw-lt">Console</span><span class="gw-help">Serial log, read only</span></span><span class="gw-value">${pick((p) => p.console)}</span></div>
      <div class="gw-row"><span class="gw-label"><span class="gw-lt">Input only</span></span><span class="gw-value">${pick((p) => p.inputOnly && !p.reserved)}</span></div>
      <div class="gw-row"><span class="gw-label"><span class="gw-lt">ADC</span></span><span class="gw-value">${pick((p) => p.adc && !p.reserved)}</span></div>`;
  }

  render() {
    const i = this.info;
    if (!i) return this.err ? html`<p class="gw-note error">${this.err}</p>` : html`<div class="gw-empty">Loading\u2026</div>`;
    const r = this.pinRes;
    return html`
      ${this.err ? html`<p class="gw-note error">${this.err}</p>` : nothing}
      <div class="gw-card-title">I2C scan</div>
      <div class="gw-inline">
        <label>SDA <input id="gw-sda" type="number" min="0" max="63" .value=${String(this.pref("sda", i.sda))} /></label>
        <label>SCL <input id="gw-scl" type="number" min="0" max="63" .value=${String(this.pref("scl", i.scl))} /></label>
        <button class="gw-btn" ?disabled=${this.busy} @click=${this.i2c}>Scan</button>
      </div>
      ${this.scan
        ? html`<div class="gw-listbox">
            ${this.scan.found.length
              ? this.scan.found.map(
                  (a) => html`<div class="gw-row list"><span class="gw-mark"></span>
                    <span class="gw-label"><b>0x${a.toString(16).padStart(2, "0")}</b>&nbsp; ${GW_I2C_NAMES[a] || "unknown device"}</span></div>`
                )
              : html`<div class="gw-row list"><span class="gw-mark"></span><span class="gw-label">No devices</span></div>`}
          </div>`
        : nothing}

      <div class="gw-card-title">GPIO</div>
      <div class="gw-inline">
        <label>Pin <input id="gw-pin" type="number" min="0" max="63" .value=${String(this.pref("pin", 0))} /></label>
        ${["read", "pullup", "pulldown", "high", "low", "adc"].map(
          (op) => html`<button class="gw-btn" ?disabled=${this.busy} @click=${() => this.gpio(op)}>
            ${{ read: "Read", pullup: "Pull-up", pulldown: "Pull-down", high: "High", low: "Low", adc: "ADC" }[op]}</button>`
        )}
      </div>
      ${r
        ? html`<p class="gw-note">GPIO ${r.pin}: <b>${r.mv !== undefined ? `${r.mv} mV` : r.level ? "HIGH" : "LOW"}</b>
            ${r.op === "high" || r.op === "low" ? " (driving)" : r.op === "pullup" ? " (with pull-up)" : r.op === "pulldown" ? " (with pull-down)" : ""}</p>`
        : nothing}

      <div class="gw-card-title">Inputs</div>
      <div class="gw-buttons">
        <button class="gw-btn" ?disabled=${this.busy} @click=${this.readInputs}>Read All Pins</button>
      </div>
      ${this.inputs
        ? html`<div class="gw-pins">${this.inputs.map(
            (p) => html`<span class=${p.level ? "hi" : "lo"}><b>${p.n}</b> ${p.level ? "H" : "L"}</span>`
          )}</div>
          <p class="gw-note">Sets every free pin to input, then reads it once.</p>`
        : nothing}

      <div class="gw-card-title">Pins on this chip</div>
      ${this.pinSummary()}`;
  }
}
customElements.define("gw-hw", GwHw);

// ---- <gw-time> -------------------------------------------------------------------------

class GwTime extends GwElement {
  static properties = { note: { state: true } };

  async setFromBrowser() {
    try {
      await gwApi.post("/api/time", { epoch: Math.floor(Date.now() / 1000) });
      this.note = { text: "Clock set" };
    } catch (e) {
      this.note = { error: true, text: e.message };
    }
  }

  render() {
    return html`
      <div class="gw-buttons">
        <button class="gw-btn" @click=${this.setFromBrowser}>Use Browser Time</button>
      </div>
      ${this.note ? html`<p class="gw-note ${this.note.error ? "error" : ""}">${this.note.text}</p>` : nothing}`;
  }
}
customElements.define("gw-time", GwTime);

// ---- <gw-img> ----------------------------------------------------------------
// Live image (MJPEG stream or a plain picture) for GsGroup.widget "img:<src>".
// A src starting with ':' is a port on the device's own host. While the
// tab is hidden the <img> is removed, which closes a stream connection -
// a camera stream would otherwise keep the device busy for nobody.

class GwImg extends GwElement {
  static properties = { spec: {}, visible: { state: true }, failed: { state: true } };

  constructor() {
    super();
    this.visible = document.visibilityState === "visible";
    this.onVis = () => {
      this.visible = document.visibilityState === "visible";
      if (this.visible) this.failed = false;
    };
  }

  connectedCallback() {
    super.connectedCallback();
    document.addEventListener("visibilitychange", this.onVis);
  }

  disconnectedCallback() {
    super.disconnectedCallback();
    document.removeEventListener("visibilitychange", this.onVis);
  }

  get src() {
    const s = this.spec || "";
    return s.startsWith(":") ? `${location.protocol}//${location.hostname}${s}` : s;
  }

  render() {
    return html`<div class="gw-img">
      ${this.visible && !this.failed
        ? html`<img src=${this.src} alt="" @error=${() => (this.failed = true)} />`
        : html`<div class="gw-img-off">${this.failed ? "No image" : "Paused"}</div>`}
      ${this.failed
        ? html`<button class="gw-btn" @click=${() => (this.failed = false)}>Retry</button>`
        : nothing}
    </div>`;
  }
}
customElements.define("gw-img", GwImg);

// ---- <gw-system-extra> -------------------------------------------------------

class GwSystemExtra extends GwElement {
  static properties = { note: { state: true }, uploading: { state: true }, progress: { state: true } };

  async ota(e) {
    const file = e.target.files[0];
    e.target.value = "";
    if (!file) return;
    const ok = await gwDialog({
      title: "Update Firmware",
      text: `Install \u201c${file.name}\u201d (${Math.round(file.size / 1024)} KB) and restart? Settings are kept.`,
      ok: "Install",
    });
    if (!ok) return;
    this.note = null;
    this.uploading = true;
    this.progress = 0;
    // XHR, not fetch: fetch has no upload progress, and a 1 MB upload over
    // a weak AP link takes long enough that a progress readout matters.
    const xhr = new XMLHttpRequest();
    xhr.open("POST", "/api/ota");
    xhr.setRequestHeader("X-GW", "1");
    xhr.setRequestHeader("Content-Type", "application/octet-stream");
    xhr.upload.onprogress = (ev) => {
      if (ev.lengthComputable) this.progress = Math.round((ev.loaded * 100) / ev.total);
    };
    xhr.onload = () => {
      this.uploading = false;
      let res = null;
      try {
        res = JSON.parse(xhr.responseText);
      } catch (_) {}
      if (xhr.status === 200 && res && res.reboot) gwRebooting();
      else this.note = { error: true, text: (res && res.error) || `Update failed (${xhr.status})` };
    };
    xhr.onerror = () => {
      this.uploading = false;
      this.note = { error: true, text: "Upload interrupted" };
    };
    xhr.send(file);
  }

  async changePassword() {
    const ok = await gwDialog({
      title: "Change Web Password",
      ok: "Change",
      inputs: [
        { name: "current", type: "password", label: "Current", autocomplete: "current-password" },
        { name: "next", type: "password", label: "New", autocomplete: "new-password" },
        { name: "again", type: "password", label: "Repeat new", autocomplete: "new-password" },
      ],
      submit: async (v) => {
        if (v.next !== v.again) return "New passwords differ";
        try {
          await gwApi.post("/api/password", { current: v.current, next: v.next });
        } catch (e) {
          return e.message;
        }
      },
    });
    if (ok) this.note = { text: "Password changed" };
  }

  pickRestore() {
    this.querySelector("input[type=file]").click();
  }

  async restore(e) {
    const file = e.target.files[0];
    e.target.value = "";
    if (!file) return;
    let json;
    try {
      json = JSON.parse(await file.text());
    } catch (_) {
      this.note = { error: true, text: "Not a settings backup" };
      return;
    }
    const ok = await gwDialog({
      title: "Restore Settings?",
      text: `Settings from “${file.name}” replace the current ones and the device restarts. Passwords are not part of a backup and stay unchanged.`,
      ok: "Restore",
      danger: true,
    });
    if (!ok) return;
    try {
      await gwApi.post("/api/restore", json);
      gwRebooting();
    } catch (err) {
      this.note = { error: true, text: err.message };
    }
  }

  async logout() {
    try {
      await gwApi.post("/api/logout");
    } catch (_) {}
    location.reload();
  }

  render() {
    return html`
      ${this.note ? html`<p class="gw-note ${this.note.error ? "error" : ""}">${this.note.text}</p>` : nothing}
      <div class="gw-card-title">Web access</div>
      <div class="gw-buttons">
        <button class="gw-btn" @click=${this.changePassword}>Change Password\u2026</button>
        <button class="gw-btn" @click=${this.logout}>Sign Out</button>
      </div>
      <div class="gw-card-title">Settings backup</div>
      <div class="gw-buttons">
        <a class="gw-btn" style="display:inline-flex;align-items:center;text-decoration:none" href="/api/backup" download>Download</a>
        <button class="gw-btn" @click=${this.pickRestore}>Restore\u2026</button>
      </div>
      <input type="file" accept="application/json,.json" hidden @change=${this.restore} />
      ${window.gwSession && window.gwSession.ota
        ? html`<div class="gw-card-title">Firmware</div>
            <div class="gw-buttons">
              <button class="gw-btn" ?disabled=${this.uploading} @click=${() => this.querySelector("#gw-ota").click()}>
                ${this.uploading ? `Uploading ${this.progress}%` : "Update\u2026"}
              </button>
            </div>
            <input id="gw-ota" type="file" accept=".bin,application/octet-stream" hidden @change=${this.ota} />`
        : nothing}
    `;
  }
}
customElements.define("gw-system-extra", GwSystemExtra);
