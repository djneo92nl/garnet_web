// garnet_web UI - shared bits: API client, icon set, dialog helper.
// Plain scripts sharing one global scope (build_ui.py concatenates them into
// the page in index.html order), with Lit exposed as window.Lit by the
// vendored bundle. No module loader, so no build step beyond inlining.

const { LitElement, html, svg, nothing } = Lit;

// Components render into light DOM so the single styles.css applies to
// everything - shadow DOM would need per-component style copies.
class GwElement extends LitElement {
  createRenderRoot() {
    return this;
  }
}

// ---- API ------------------------------------------------------------------

class GwApiError extends Error {
  constructor(message, status, data) {
    super(message);
    this.status = status;
    this.field = data && data.field;
    this.data = data;
  }
}

async function gwHandle(res) {
  let data = null;
  try {
    data = await res.json();
  } catch (_) {
    // non-JSON (e.g. device mid-reboot) - fall through with null
  }
  // 401 without a field = session gone (reboot / expiry): back to login.
  // 401 *with* a field is a wrong password on the login form itself.
  if (res.status === 401 && !(data && data.field)) {
    window.dispatchEvent(new CustomEvent("gw-unauth"));
  }
  if (!res.ok) throw new GwApiError((data && data.error) || res.statusText, res.status, data);
  return data;
}

const gwApi = {
  get(path) {
    return fetch(path, { credentials: "same-origin", cache: "no-store" }).then(gwHandle);
  },
  post(path, body) {
    return fetch(path, {
      method: "POST",
      credentials: "same-origin",
      // X-GW: the device rejects POSTs without it (cheap CSRF guard - a
      // cross-site form can't set custom headers).
      headers: { "Content-Type": "application/json", "X-GW": "1" },
      body: JSON.stringify(body || {}),
    }).then(gwHandle);
  },
};

// Tell the app shell the device is restarting (shows the overlay).
function gwRebooting() {
  window.dispatchEvent(new CustomEvent("gw-reboot"));
}

// ---- Icons ------------------------------------------------------------------
// Group `icon` names from GsGroup.icon, drawn as plain 16px line glyphs in
// the text color (flat, like garnet_ui's own). Unknown names get "sliders".

const GW_ICONS = {
  wifi: svg`<path d="M2 8.5a15 15 0 0 1 20 0M5 12a10 10 0 0 1 14 0M8.5 15.5a5 5 0 0 1 7 0"/><circle cx="12" cy="19" r="1.2" fill="currentColor"/>`,
  ethernet: svg`<path d="M5 4h14v11h-3v3H8v-3H5z"/><path d="M9 8v3M12 8v3M15 8v3"/>`,
  bluetooth: svg`<path d="M7 7l10 10-5 5V2l5 5L7 17"/>`,
  sdcard: svg`<path d="M8 2h8l4 4v16H4V6z"/><path d="M10 6v3M13 6v3M16 6v3"/>`,
  system: svg`<rect x="6" y="6" width="12" height="12" rx="1"/><path d="M9 2v4M15 2v4M9 18v4M15 18v4M2 9h4M2 15h4M18 9h4M18 15h4"/>`,
  display: svg`<rect x="3" y="4" width="18" height="12" rx="1"/><path d="M8 20h8M12 16v4"/>`,
  power: svg`<path d="M12 3v8"/><path d="M6.3 6.3a8 8 0 1 0 11.4 0"/>`,
  bolt: svg`<path d="M13 2L4 14h7l-1 8 9-12h-7z"/>`,
  clock: svg`<circle cx="12" cy="12" r="9"/><path d="M12 7v5l3 2"/>`,
  bell: svg`<path d="M6 16V11a6 6 0 0 1 12 0v5l2 2H4z"/><path d="M10 21h4"/>`,
  lock: svg`<rect x="5" y="11" width="14" height="10" rx="1"/><path d="M8 11V7a4 4 0 0 1 8 0v4"/>`,
  camera: svg`<path d="M3 7h4l2-3h6l2 3h4v13H3z"/><circle cx="12" cy="13" r="4"/>`,
  sensor: svg`<path d="M14 14.8V4a2 2 0 0 0-4 0v10.8a4 4 0 1 0 4 0z"/>`,
  light: svg`<path d="M9 18h6M10 21h4M12 3a6 6 0 0 0-4 10.5c.8.8 1 1.5 1 2.5h6c0-1 .2-1.7 1-2.5A6 6 0 0 0 12 3z"/>`,
  folder: svg`<path d="M3 6h7l2 2h9v11H3z"/>`,
  file: svg`<path d="M6 3h8l4 4v14H6z"/><path d="M14 3v4h4"/>`,
  sliders: svg`<path d="M4 6h10M18 6h2M4 12h4M12 12h8M4 18h12"/><circle cx="16" cy="6" r="2"/><circle cx="10" cy="12" r="2"/><circle cx="18" cy="18" r="2"/>`,
};

function gwIcon(name) {
  return html`<span class="gw-glyph"
    ><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"
      stroke-linecap="round" stroke-linejoin="round">${GW_ICONS[name] || GW_ICONS.sliders}</svg></span
  >`;
}

const gwMenuIcon = svg`<path d="M3 6h18M3 12h18M3 18h18"/>`;
const gwLockIcon = html`<svg class="gw-lock" viewBox="0 0 24 24" fill="currentColor"
  ><path d="M7 10V7a5 5 0 0 1 10 0v3h1a1 1 0 0 1 1 1v10a1 1 0 0 1-1 1H6a1 1 0 0 1-1-1V11a1 1 0 0 1 1-1zm2 0h6V7a3 3 0 0 0-6 0z"/></svg>`;

// ---- Dialog -----------------------------------------------------------------
// One <gw-dialog> lives in the app shell. gwDialog({...}) opens it and
// resolves with the input values (or true when there are no inputs) on
// OK, or null on Cancel. `submit(values)` may return an error string to
// keep the dialog open (e.g. wrong password) - or null/undefined to close.

class GwDialog extends GwElement {
  static properties = { opts: { state: true }, error: { state: true }, busy: { state: true } };

  constructor() {
    super();
    this.opts = null;
    GwDialog.instance = this;
  }

  open(opts) {
    this.opts = opts;
    this.error = "";
    this.busy = false;
    return new Promise((resolve) => (this.resolve = resolve));
  }

  close(result) {
    this.opts = null;
    if (this.resolve) this.resolve(result);
    this.resolve = null;
  }

  async ok(e) {
    e.preventDefault();
    const values = {};
    for (const input of this.querySelectorAll("input[name]")) values[input.name] = input.value;
    const result = (this.opts.inputs || []).length ? values : true;
    if (this.opts.submit) {
      this.busy = true;
      this.error = "";
      try {
        const err = await this.opts.submit(values);
        if (err) {
          this.error = err;
          return;
        }
      } finally {
        this.busy = false;
      }
    }
    this.close(result);
  }

  updated() {
    const first = this.querySelector("input[name]");
    if (first && this.opts && !this.focused) {
      first.focus();
      this.focused = true;
    }
    if (!this.opts) this.focused = false;
  }

  render() {
    const o = this.opts;
    if (!o) return nothing;
    // Palm alert: title bar, message, fields, buttons left-aligned with
    // the default action first.
    return html`<div class="gw-modal" @click=${(e) => e.target === e.currentTarget && this.close(null)}>
      <form class="gw-dialog" @submit=${this.ok}>
        <h2>${o.title}</h2>
        <div class="gw-dialog-body">
          ${o.text ? html`<p>${o.text}</p>` : nothing}
          ${(o.inputs || []).map(
            (i) => html`<label>${i.label}<input name=${i.name} type=${i.type || "text"}
              .value=${i.value || ""} autocomplete=${i.autocomplete || "off"} autocapitalize="off" /></label>`
          )}
          ${this.error ? html`<div class="gw-err">${this.error}</div>` : nothing}
          <div class="gw-dialog-actions">
            <button type="submit" class="gw-btn primary ${o.danger ? "danger" : ""}" ?disabled=${this.busy}>
              ${o.ok || "OK"}
            </button>
            <button type="button" class="gw-btn" @click=${() => this.close(null)}>Cancel</button>
          </div>
        </div>
      </form>
    </div>`;
  }
}
customElements.define("gw-dialog", GwDialog);

function gwDialog(opts) {
  return GwDialog.instance.open(opts);
}
