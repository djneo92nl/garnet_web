// <gw-app> - shell: login gate, sidebar, routing (#groupId), reboot overlay.

// Built-in sections bracket the app's own: connections first (like
// iPadOS), device/about last, everything the app registered in between.
const GW_SECTION_FIRST = "Connections";
const GW_SECTION_LAST = "Device";

class GwApp extends GwElement {
  static properties = {
    session: { state: true },
    groups: { state: true },
    current: { state: true },
    drawer: { state: true },
    rebooting: { state: true },
    loginError: { state: true },
    loginBusy: { state: true },
    fatal: { state: true },
  };

  constructor() {
    super();
    this.groups = null;
    window.addEventListener("hashchange", () => this.route());
    window.addEventListener("gw-unauth", () => {
      if (this.session) this.session = { ...this.session, auth: false };
    });
    window.addEventListener("gw-reboot", () => this.startReboot());
  }

  connectedCallback() {
    super.connectedCallback();
    this.boot();
  }

  async boot() {
    try {
      this.session = await gwApi.get("/api/session");
    } catch (e) {
      this.fatal = "Can't reach the device.";
      return;
    }
    // GwConfig.accent replaces the navy/sky-blue tag color in both themes.
    if (this.session.accent) document.documentElement.style.setProperty("--tag", this.session.accent);
    document.title = this.session.name;
    if (this.session.auth) await this.loadSchema();
  }

  async loadSchema() {
    try {
      this.groups = await gwApi.get("/api/schema");
    } catch (e) {
      if (e.status !== 401) this.fatal = e.message;
      return;
    }
    this.route();
  }

  route() {
    if (!this.groups || !this.groups.length) return;
    const id = decodeURIComponent(location.hash.slice(1));
    const ordered = this.sections().flatMap((s) => s.groups);
    this.current = ordered.find((g) => g.id === id) || ordered[0];
    this.drawer = false;
  }

  go(g) {
    const main = this.querySelector(".gw-group-host");
    if (main && main.dirty && !confirm("Discard unsaved changes?")) return;
    location.hash = g.id; // -> hashchange -> route()
    if (this.current && this.current.id === g.id) this.drawer = false;
  }

  sections() {
    const map = new Map();
    for (const g of this.groups) {
      const name = g.section || "Settings";
      if (!map.has(name)) map.set(name, []);
      map.get(name).push(g);
    }
    const rank = (n) => (n === GW_SECTION_FIRST ? 0 : n === GW_SECTION_LAST ? 2 : 1);
    return [...map.entries()]
      .map(([name, groups], i) => ({ name, groups, i }))
      .sort((a, b) => rank(a.name) - rank(b.name) || a.i - b.i);
  }

  async login(e) {
    e.preventDefault();
    this.loginBusy = true;
    this.loginError = "";
    const password = this.querySelector("#gw-pw").value;
    try {
      await gwApi.post("/api/login", { password });
      this.session = { ...this.session, auth: true };
      await this.loadSchema();
    } catch (err) {
      this.loginError = err.message;
    } finally {
      this.loginBusy = false;
    }
  }

  // Wait for the device to go away and come back, then reload. After a
  // network change it may come back on another address - hence the hint.
  startReboot() {
    this.rebooting = true;
    const started = Date.now();
    let wentDown = false;
    const check = async () => {
      try {
        const ctl = new AbortController();
        const t = setTimeout(() => ctl.abort(), 2500);
        const res = await fetch("/api/session", { cache: "no-store", signal: ctl.signal });
        clearTimeout(t);
        if (res.ok && (wentDown || Date.now() - started > 8000)) {
          location.reload();
          return;
        }
      } catch (_) {
        wentDown = true;
      }
      setTimeout(check, 2000);
    };
    setTimeout(check, 2500);
  }

  renderLogin() {
    return html`<div class="gw-login">
      <form class="gw-dialog" @submit=${this.login}>
        <h2>${this.session.name}</h2>
        <div class="gw-dialog-body">
          <label>Password<input id="gw-pw" type="password" autocomplete="current-password" autofocus /></label>
          ${this.loginError ? html`<div class="gw-err">${this.loginError}</div>` : nothing}
          <div class="gw-dialog-actions">
            <button class="gw-btn primary" type="submit" ?disabled=${this.loginBusy}>Sign In</button>
          </div>
        </div>
      </form>
    </div>`;
  }

  renderSidebar() {
    return html`<nav class="gw-sidebar" aria-label="Settings groups">
      ${this.sections().map(
        (sec) => html`<div class="gw-nav-section">
          <h3>${sec.name}</h3>
          <div class="gw-nav">
            ${sec.groups.map(
              (g) => html`<button class=${this.current && this.current.id === g.id ? "active" : ""}
                @click=${() => this.go(g)}>${gwIcon(g.icon || g.id)}<span>${g.title}</span></button>`
            )}
          </div>
        </div>`
      )}
    </nav>`;
  }

  render() {
    const s = this.session;
    if (this.fatal) return html`<div class="gw-empty">${this.fatal}</div>`;
    if (!s) return html`<div class="gw-empty">Loading\u2026</div>`;
    if (this.rebooting) {
      return html`<div class="gw-modal"><div class="gw-dialog">
        <h2>Restarting</h2>
        <div class="gw-dialog-body">
          <p>This page reloads when the device is back.</p>
          <p>New network? Use <b>http://${s.host || "device"}.local</b></p>
        </div>
      </div></div>`;
    }
    if (!s.auth) return html`${this.renderLogin()}<gw-dialog></gw-dialog>`;
    if (!this.groups) return html`<div class="gw-empty">Loading\u2026</div>`;

    return html`<div class="gw-shell ${this.drawer ? "open" : ""}">
        <div class="gw-titlebar">
          <button class="gw-icon-btn gw-menu-btn" aria-label="Show groups" @click=${() => (this.drawer = true)}>
            <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round">${gwMenuIcon}</svg>
          </button>
          <span class="gw-tag">${s.logo ? html`<span class="gw-logo" .innerHTML=${s.logo}></span>` : nothing}${s.name}</span>
          <span class="gw-spacer"></span>
          <span class="gw-where">${s.host ? `${s.host}.local` : ""}</span>
        </div>
        ${this.renderSidebar()}
        <div class="gw-scrim" @click=${() => (this.drawer = false)}></div>
        <main class="gw-main">
          <div class="gw-main-inner">
            ${this.current
              ? html`<gw-group class="gw-group-host" .group=${this.current}></gw-group>`
              : html`<div class="gw-empty">No settings registered.</div>`}
          </div>
        </main>
      </div>
      <gw-dialog></gw-dialog>`;
  }
}
customElements.define("gw-app", GwApp);
