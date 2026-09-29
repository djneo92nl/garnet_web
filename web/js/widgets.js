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
  static properties = { note: { state: true } };

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
    `;
  }
}
customElements.define("gw-system-extra", GwSystemExtra);
