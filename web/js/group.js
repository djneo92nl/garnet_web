// <gw-group> - the right-hand column: one garnet_settings group as iOS
// inset cards, with Save / Cancel for the whole group.

const GW_EDITABLE = new Set(["toggle", "text", "password", "number", "select"]);
const GW_POLL_MS = 2000;

class GwGroup extends GwElement {
  static properties = {
    group: { attribute: false },
    values: { state: true },
    edits: { state: true },
    errors: { state: true },
    busy: { state: true },
    message: { state: true },
  };

  constructor() {
    super();
    this.values = null;
    this.edits = {};
    this.errors = {};
    this.message = null;
  }

  connectedCallback() {
    super.connectedCallback();
    this.onVisible = () => this.schedulePoll();
    document.addEventListener("visibilitychange", this.onVisible);
  }

  disconnectedCallback() {
    super.disconnectedCallback();
    document.removeEventListener("visibilitychange", this.onVisible);
    clearTimeout(this.pollTimer);
  }

  willUpdate(changed) {
    if (changed.has("group") && this.group) {
      this.values = null;
      this.edits = {};
      this.errors = {};
      this.message = null;
      this.load();
    }
  }

  get dirty() {
    return Object.keys(this.edits).length > 0;
  }

  async load() {
    const id = this.group.id;
    try {
      const values = await gwApi.get(`/api/group/${id}`);
      if (this.group.id === id) this.values = values;
    } catch (e) {
      this.message = { error: true, text: e.message };
    }
    this.schedulePoll();
  }

  // Live Info values: poll only while the tab is visible and the group
  // actually has Info rows - a phone left open on the page shouldn't keep
  // the device's radio busy.
  schedulePoll() {
    clearTimeout(this.pollTimer);
    if (!this.group || document.visibilityState !== "visible") return;
    if (!this.group.fields.some((f) => f.type === "info")) return;
    this.pollTimer = setTimeout(() => this.poll(), GW_POLL_MS);
  }

  async poll() {
    const id = this.group.id;
    try {
      const live = await gwApi.get(`/api/live/${id}`);
      if (this.group.id === id && this.values) this.values = { ...this.values, ...live };
    } catch (_) {
      // transient (device busy / rebooting) - try again next tick
    }
    this.schedulePoll();
  }

  value(key) {
    return key in this.edits ? this.edits[key] : this.values ? this.values[key] : undefined;
  }

  edit(field, v) {
    const edits = { ...this.edits };
    const original = this.values ? this.values[field.key] : undefined;
    // Password: server value is a boolean "is set"; "" = keep current.
    if (field.type === "password" ? v === "" : v === original) delete edits[field.key];
    else edits[field.key] = v;
    this.edits = edits;
    if (this.errors[field.key]) this.errors = { ...this.errors, [field.key]: undefined };
    this.message = null;
  }

  visible(field) {
    if (!field.showIf) return true;
    const neg = field.showIf.startsWith("!");
    const on = !!this.value(neg ? field.showIf.slice(1) : field.showIf);
    return neg ? !on : on;
  }

  cancel() {
    this.edits = {};
    this.errors = {};
    this.message = null;
  }

  async save() {
    this.busy = true;
    this.message = null;
    try {
      const res = await gwApi.post(`/api/group/${this.group.id}`, this.edits);
      if (res && res.reboot) {
        gwRebooting();
        return;
      }
      this.edits = {};
      this.errors = {};
      this.message = { text: "Saved" };
      await this.load();
    } catch (e) {
      if (e.field) this.errors = { [e.field]: e.message };
      else this.message = { error: true, text: e.message };
    } finally {
      this.busy = false;
    }
  }

  async runAction(field) {
    if (field.confirm) {
      const ok = await gwDialog({ title: field.label, text: field.confirm, ok: field.label });
      if (!ok) return;
    }
    try {
      await gwApi.post(`/api/action/${this.group.id}/${field.key}`);
      this.message = { text: `${field.label}: done` };
    } catch (e) {
      this.message = { error: true, text: e.message };
    }
  }

  // ---- Rendering ----------------------------------------------------------

  renderControl(f) {
    const v = this.value(f.key);
    const err = !!this.errors[f.key];
    switch (f.type) {
      case "toggle":
        return html`<label class="gw-check-box"
          ><input type="checkbox" .checked=${!!v} aria-label=${f.label}
            @change=${(e) => this.edit(f, e.target.checked)}
        /></label>`;
      case "select":
        return html`<select aria-label=${f.label} @change=${(e) => this.edit(f, Number(e.target.value))}>
          ${f.options.map((o, i) => html`<option value=${i} ?selected=${i === v}>${o}</option>`)}
        </select>`;
      case "number":
        return html`<input type="number" inputmode="numeric" min=${f.min} max=${f.max} step=${f.step}
          class=${err ? "invalid" : ""} aria-label=${f.label} .value=${v === undefined ? "" : String(v)}
          @input=${(e) => {
            const n = Number(e.target.value);
            if (e.target.value !== "" && Number.isInteger(n)) this.edit(f, n);
          }} />`;
      case "password":
        return html`<input type="password" autocomplete="new-password" maxlength=${f.maxLen}
          class=${err ? "invalid" : ""} aria-label=${f.label}
          placeholder=${this.values && this.values[f.key] ? "••••••" : "Not set"}
          .value=${typeof this.edits[f.key] === "string" ? this.edits[f.key] : ""}
          @input=${(e) => this.edit(f, e.target.value)} />`;
      case "text":
        return html`<input type="text" autocapitalize="off" autocorrect="off" spellcheck="false"
          maxlength=${f.maxLen} placeholder=${f.placeholder || ""} class=${err ? "invalid" : ""}
          aria-label=${f.label} .value=${v === undefined ? "" : v}
          @input=${(e) => this.edit(f, e.target.value)} />`;
      default:
        return html`<span class="gw-value">${v === undefined || v === "" ? "–" : v}</span>`;
    }
  }

  renderRow(f) {
    return html`<div class="gw-row">
      <span class="gw-label"
        ><span class="gw-lt">${f.label}</span>${this.errors[f.key]
          ? html`<span class="gw-err">${this.errors[f.key]}</span>`
          : f.help
          ? html`<span class="gw-help">${f.help}</span>`
          : nothing}</span
      >
      ${this.renderControl(f)}
    </div>`;
  }

  // Consecutive fields of the same kind (live info / editable / buttons)
  // share a card - the way iOS Settings separates "about" rows from
  // controls from actions.
  cards() {
    const kind = (f) => (f.type === "info" ? "info" : f.type === "button" ? "button" : "edit");
    const cards = [];
    for (const f of this.group.fields) {
      if (!this.visible(f)) continue;
      const last = cards[cards.length - 1];
      if (last && last.kind === kind(f)) last.fields.push(f);
      else cards.push({ kind: kind(f), fields: [f] });
    }
    return cards;
  }

  // A widget goes after a leading status (Info) card - "what is it doing
  // now" first, then the widget's controls, then the settings.
  renderCards(widget) {
    const cards = this.cards();
    const at = cards.length && cards[0].kind === "info" ? 1 : 0;
    const card = (c) =>
      c.kind === "button"
        ? html`<div class="gw-card gw-buttons">${c.fields.map(
            (f) => html`<button class="gw-btn" title=${f.help || ""} @click=${() => this.runAction(f)}>${f.label}</button>`
          )}</div>`
        : html`<div class="gw-card">${c.fields.map((f) => this.renderRow(f))}</div>`;
    return html`${cards.slice(0, at).map(card)}${widget}${cards.slice(at).map(card)}`;
  }

  render() {
    const g = this.group;
    if (!g) return nothing;
    const widget = g.widget === "wifi" ? html`<gw-wifi></gw-wifi>` : nothing;
    const after = g.widget === "system" ? html`<gw-system-extra></gw-system-extra>` : nothing;
    return html`
      <div class="gw-header">
        <div class="gw-titlebar">
          <span class="gw-tag">${g.title}</span>
          <span class="gw-spacer"></span>
          ${this.dirty
            ? html`<div class="gw-actions">
                <button class="gw-btn primary" @click=${this.save} ?disabled=${this.busy}>
                  ${g.reboot ? "Save & Restart" : "Save"}
                </button>
                <button class="gw-btn" @click=${this.cancel} ?disabled=${this.busy}>Cancel</button>
              </div>`
            : nothing}
        </div>
        ${this.message
          ? html`<p class="gw-note ${this.message.error ? "error" : ""}">${this.message.text}</p>`
          : nothing}
      </div>
      ${this.values === null ? html`<div class="gw-empty">Loading\u2026</div>` : this.renderCards(widget)}
      ${after}
    `;
  }
}
customElements.define("gw-group", GwGroup);
