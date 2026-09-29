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
