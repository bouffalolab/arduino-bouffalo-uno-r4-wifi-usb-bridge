// Isometric SVG drawing of an UNO R4 WiFi whose ESP32-S3 module is replaced by BL616CL.
// Units are millimetres. x runs from the USB-C edge to the right; y runs from the
// digital-header edge toward the power header. Main part positions come from the
// official ABX00087 Altium layout; small passives are omitted.

const PHI = (34 * Math.PI) / 180;
const SQ = Math.sin(PHI);
const ZK = Math.cos(PHI);
const CX = 34.29;
const CY = 26.67;
const PCB_Z = 1.6;

const C = {
  pcb: '#0b8d92',
  pcbEdge: '#0a5d61',
  silk: '#e8f6f4',
  copper: '#d9b45a',
  black: '#1d2124',
  chip: '#23272b',
  metal: '#c9d1d5',
  white: '#ece6d6',
  module: '#183c48',
};

export const PATHS = {
  usb: { color: '#2a78d6', pts: [[5.9, 15.47], [13.6, 15.47], [13.6, 24.07]] },
  serial: { color: '#eb6834', pts: [[20.9, 24.07], [20.9, 15.6], [47.2, 15.6]] },
  at: { color: '#1baf7a', pts: [[16.5, 24.07], [16.5, 13.8], [47.2, 13.8]] },
  ctrl: { color: '#eda100', pts: [[17.8, 24.07], [17.8, 14.4], [47.2, 14.4]] },
  swd: { color: '#e87ba4', pts: [[19.2, 24.07], [19.2, 15.0], [47.2, 15.0]] },
  matrix: { color: '#008300', pts: [[50.5, 22.6], [50.5, 26.0]] },
};

// Anchors used by hotspots, labels and off-board links: [x, y, z].
export const ANCHORS = {
  usb: [2.5, 15.47, 4.9],
  bl: [13.6, 31.8, 4.6],
  antenna: [3.0, 31.8, 2.6],
  ra: [52.75, 17.49, 3.0],
  u4: [18.7, 20.04, 2.6],
  mux: [12.53, 15.47, 2.4],
  matrix: [45.96, 34.16, 1.9],
  j6: [18.8, 11.38, 1.7],
  reset: [6.35, 3.94, 4.6],
  dc: [5.24, 45.18, 12.6],
  digital: [40.0, 2.54, 10.1],
  power: [36.8, 50.8, 10.1],
  icsp: [64.9, 25.4, 8.0],
  qwiic: [65.74, 38.99, 5.8],
  leds: [26.42, 16.76, 2.0],
};

const OUTLINE = [
  [0, 1], [0.29, 0.29], [1, 0], [64.52, 0], [66.04, 1.52], [66.04, 12.95], [68.58, 15.49],
  [68.58, 48.26], [66.04, 50.8], [66.04, 52.34], [65.75, 53.05], [65.04, 53.34], [1, 53.34],
  [0.29, 53.05], [0, 52.34],
];
const HOLES = [[15.24, 2.54], [13.97, 50.8], [66.04, 17.78], [66.04, 45.72]];
const DIGITAL = ['SCL', 'SDA', 'AREF', 'GND', '13', '12', '~11', '~10', '~9', '8', '7', '~6', '~5', '4', '~3', '2', 'TX 1', 'RX 0'];
const POWER = ['BOOT', 'IOREF', 'RESET', '3.3V', '5V', 'GND', 'GND', 'VIN', 'A0', 'A1', 'A2', 'A3', 'A4', 'A5'];

function digitalX(i) {
  return i < 10 ? 18.8 + 2.54 * i : 45.72 + 2.54 * (i - 10);
}

function powerX(i) {
  return i < 8 ? 27.94 + 2.54 * i : 50.8 + 2.54 * (i - 8);
}

function hexToRgb(hex) {
  const n = parseInt(hex.slice(1), 16);
  return [(n >> 16) & 255, (n >> 8) & 255, n & 255];
}

function shade(hex, f) {
  const [r, g, b] = hexToRgb(hex);
  const k = (v) => Math.max(0, Math.min(255, Math.round(f >= 1 ? v + (255 - v) * (f - 1) : v * f)));
  return `rgb(${k(r)},${k(g)},${k(b)})`;
}

function box(id, x, y, z, w, d, h, color, extra = {}) {
  return { id, kind: 'box', x, y, z, w, d, h, color, ...extra };
}

function buildParts() {
  const parts = [];
  // BL616CL module replacing ESP32-S3-MINI-1 (M1), antenna toward the board edge.
  parts.push(box('bl', 0.25, 24.07, PCB_Z, 20.65, 15.55, 0.8, C.module, { group: 'bl', top: 'antenna' }));
  parts.push(box('bl', 6.6, 24.6, PCB_Z + 0.8, 13.7, 14.5, 1.6, C.metal, { group: 'bl', top: 'blcan', metal: true }));
  parts.push(box('usb', -0.86, 10.7, PCB_Z, 6.8, 9.54, 3.2, C.metal, { group: 'usb', metal: true }));
  parts.push(box('usb', -0.92, 11.9, PCB_Z + 0.75, 0.06, 7.1, 1.7, '#30363a', { group: 'usb' }));
  parts.push(box('dc', -1.96, 40.48, PCB_Z, 14.4, 9.4, 11, C.black, { group: 'dc', top: 'dc' }));
  parts.push(box('reset', 3.35, 0.94, PCB_Z, 6, 6, 1.6, C.metal, { group: 'reset', metal: true }));
  parts.push(box('reset', 4.85, 2.44, PCB_Z + 1.6, 3, 3, 1.4, '#3b4045', { group: 'reset' }));
  parts.push(box('ra', 47.75, 12.49, PCB_Z, 10, 10, 1.4, C.chip, { group: 'ra', top: 'ra' }));
  parts.push(box('u4', 17.2, 19.29, PCB_Z, 3, 1.5, 0.9, C.chip, { group: 'u4', top: 'u4' }));
  parts.push(box('mux', 11.83, 13.37, PCB_Z, 1.4, 1.4, 0.7, C.chip, { group: 'mux' }));
  parts.push(box('mux', 11.83, 16.17, PCB_Z, 1.4, 1.4, 0.7, C.chip, { group: 'mux' }));
  parts.push(box('buck', 25.33, 30.24, PCB_Z, 3, 2, 0.9, C.chip, { group: 'power-parts' }));
  parts.push(box('ind', 27.11, 33.69, PCB_Z, 4.8, 4.8, 2.2, '#3a3f44', { group: 'power-parts', top: 'ind' }));
  parts.push(box('ldo', 23.48, 40.13, PCB_Z, 6.2, 6.0, 1.6, C.chip, { group: 'power-parts' }));
  parts.push(box('fuse', 7.43, 18.34, PCB_Z, 3.0, 4.6, 1.0, '#c8a24a', { group: 'power-parts' }));
  parts.push(box('diode', 14.4, 43.2, PCB_Z, 2, 3.6, 1.1, C.chip, { group: 'power-parts' }));
  parts.push(box('diode', 25.4, 21.0, PCB_Z, 2, 3.6, 1.1, C.chip, { group: 'power-parts' }));
  // Female headers (2.54 mm pitch).
  parts.push(box('digital', 17.53, 1.27, PCB_Z, 25.4, 2.54, 8.5, C.black, { group: 'headers', top: 'hdr', pins: 10, first: 18.8 }));
  parts.push(box('digital', 44.45, 1.27, PCB_Z, 20.32, 2.54, 8.5, C.black, { group: 'headers', top: 'hdr', pins: 8, first: 45.72 }));
  parts.push(box('power', 26.67, 49.53, PCB_Z, 20.32, 2.54, 8.5, C.black, { group: 'headers', top: 'hdr', pins: 8, first: 27.94 }));
  parts.push(box('power', 49.53, 49.53, PCB_Z, 15.24, 2.54, 8.5, C.black, { group: 'headers', top: 'hdr', pins: 6, first: 50.8 }));
  // ICSP: plastic base and six male pins.
  parts.push(box('icsp', 62.36, 21.59, PCB_Z, 5.08, 7.62, 2.5, C.black, { group: 'icsp' }));
  for (const px of [63.63, 66.17]) {
    for (const py of [22.86, 25.4, 27.94]) {
      parts.push(box('icsp', px - 0.32, py - 0.32, PCB_Z + 2.5, 0.64, 0.64, 6, C.copper, { group: 'icsp', metal: true }));
    }
  }
  parts.push(box('qwiic', 63.27, 35.99, PCB_Z, 4.95, 6.0, 2.9, C.white, { group: 'qwiic' }));
  return parts;
}

const PARTS = buildParts();

export class Board {
  constructor(svg, { onHotspot } = {}) {
    this.svg = svg;
    this.onHotspot = onHotspot;
    this.theta = -28;
    this.zoomK = 1;
    this.pan = [0, 0];
    this.focus = { parts: [], paths: [], actors: [], labels: [] };
    this.labelText = {};
    this.actorText = {};
    this.matrix = new Array(96).fill(0);
    this.showLabels = true;
    this.showPaths = true;
    this.fx = [];
    this.svg.innerHTML = this.defs() + '<g class="scene"></g><g class="overlay"></g><g class="fx"></g>';
    this.scene = this.svg.querySelector('.scene');
    this.overlay = this.svg.querySelector('.overlay');
    this.fxLayer = this.svg.querySelector('.fx');
    this.resize();
    new ResizeObserver(() => this.resize()).observe(this.svg);
    this.bindInput();
  }

  defs() {
    return `<defs>
      <radialGradient id="ledGlow"><stop offset="0" stop-color="#ff6a5c" stop-opacity="0.95"/><stop offset="0.45" stop-color="#ff3b30" stop-opacity="0.45"/><stop offset="1" stop-color="#ff3b30" stop-opacity="0"/></radialGradient>
      <radialGradient id="pktGlow"><stop offset="0" stop-color="#fff" stop-opacity="1"/><stop offset="0.35" stop-color="#fff" stop-opacity="0.6"/><stop offset="1" stop-color="#fff" stop-opacity="0"/></radialGradient>
      <linearGradient id="canSheen" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#eef2f4"/><stop offset="0.5" stop-color="#c3cbd0"/><stop offset="1" stop-color="#dfe5e8"/></linearGradient>
    </defs>`;
  }

  resize() {
    const r = this.svg.getBoundingClientRect();
    this.W = Math.max(320, r.width);
    this.H = Math.max(260, r.height);
    this.svg.setAttribute('viewBox', `0 0 ${this.W} ${this.H}`);
    this.baseK = Math.min(this.W / 94, this.H / 68);
    this.render();
  }

  get k() {
    return this.baseK * this.zoomK;
  }

  // World (mm) to screen pixels, plus depth (larger is nearer the viewer).
  project(x, y, z) {
    const t = (this.theta * Math.PI) / 180;
    const c = Math.cos(t);
    const s = Math.sin(t);
    const dx = x - CX;
    const dy = y - CY;
    const xr = dx * c - dy * s;
    const yr = dx * s + dy * c;
    return [this.W / 2 + this.pan[0] + this.k * xr, this.H / 2 + this.pan[1] + this.k * (yr * SQ - z * ZK) + this.k * 2, yr];
  }

  // SVG matrix mapping plane coordinates at height z to the screen.
  planeMatrix(z) {
    const [ox, oy] = this.project(0, 0, z);
    const [ax, ay] = this.project(1, 0, z);
    const [bx, by] = this.project(0, 1, z);
    return `matrix(${ax - ox} ${ay - oy} ${bx - ox} ${by - oy} ${ox} ${oy})`;
  }

  setFocus(focus) {
    this.focus = { parts: [], paths: [], actors: [], labels: [], ...focus };
    this.render();
  }

  setMatrix(bits) {
    this.matrix = bits.slice();
    this.render();
  }

  setText(labels, actors) {
    this.labelText = labels;
    this.actorText = actors;
    this.render();
  }

  rotate(delta, animate = true) {
    this.animateTo({ theta: this.theta + delta }, animate);
  }

  zoom(f) {
    this.animateTo({ zoomK: Math.max(0.6, Math.min(3, this.zoomK * f)) }, true);
  }

  reset() {
    this.animateTo({ theta: -28, zoomK: 1, pan: [0, 0] }, true);
  }

  animateTo(target, animate) {
    const from = { theta: this.theta, zoomK: this.zoomK, pan: this.pan.slice() };
    const to = { ...from, ...target };
    if (!animate || matchMedia('(prefers-reduced-motion: reduce)').matches) {
      Object.assign(this, to);
      this.render();
      return;
    }
    const t0 = performance.now();
    const step = (now) => {
      const p = Math.min(1, (now - t0) / 380);
      const e = 1 - Math.pow(1 - p, 3);
      this.theta = from.theta + (to.theta - from.theta) * e;
      this.zoomK = from.zoomK + (to.zoomK - from.zoomK) * e;
      this.pan = [0, 1].map((i) => from.pan[i] + (to.pan[i] - from.pan[i]) * e);
      this.render();
      if (p < 1) requestAnimationFrame(step);
    };
    requestAnimationFrame(step);
  }

  bindInput() {
    let drag = null;
    this.svg.addEventListener('pointerdown', (e) => {
      if (e.target.closest('.hotspot')) return;
      drag = { x: e.clientX, y: e.clientY, theta: this.theta, pan: this.pan.slice(), shift: e.shiftKey || e.button === 2 };
      this.svg.setPointerCapture(e.pointerId);
      this.svg.classList.add('dragging');
    });
    this.svg.addEventListener('pointermove', (e) => {
      if (!drag) return;
      const dx = e.clientX - drag.x;
      const dy = e.clientY - drag.y;
      if (drag.shift) this.pan = [drag.pan[0] + dx, drag.pan[1] + dy];
      else this.theta = drag.theta + dx * 0.35;
      this.render();
    });
    const end = () => {
      drag = null;
      this.svg.classList.remove('dragging');
    };
    this.svg.addEventListener('pointerup', end);
    this.svg.addEventListener('pointercancel', end);
    this.svg.addEventListener('contextmenu', (e) => e.preventDefault());
    this.svg.addEventListener('wheel', (e) => {
      e.preventDefault();
      this.zoomK = Math.max(0.6, Math.min(3, this.zoomK * (e.deltaY < 0 ? 1.08 : 1 / 1.08)));
      this.render();
    }, { passive: false });
    this.svg.addEventListener('click', (e) => {
      const hs = e.target.closest('.hotspot');
      if (hs && this.onHotspot) this.onHotspot(hs.dataset.id);
    });
  }

  isFocused(group) {
    return this.focus.parts.includes(group);
  }

  render() {
    if (!this.W) return;
    const out = [];
    out.push(this.drawPcb());
    out.push(this.drawSurface());
    out.push(this.drawParts());
    this.scene.innerHTML = out.join('');
    this.overlay.innerHTML = this.drawOverlay();
    this.drawFx();
  }

  drawPcb() {
    const bottom = OUTLINE.map(([x, y]) => this.project(x, y, 0));
    const top = OUTLINE.map(([x, y]) => this.project(x, y, PCB_Z));
    const faces = [];
    for (let i = 0; i < OUTLINE.length; i++) {
      const j = (i + 1) % OUTLINE.length;
      const [x0, y0] = OUTLINE[i];
      const [x1, y1] = OUTLINE[j];
      // Outward normal for a clockwise-in-screen outline (y grows downward in plan).
      const nx = -(y1 - y0);
      const ny = x1 - x0;
      const t = (this.theta * Math.PI) / 180;
      const facing = -(nx * Math.sin(t) + ny * Math.cos(t));
      if (facing <= 0) continue;
      const pts = [bottom[i], bottom[j], top[j], top[i]].map((p) => `${p[0].toFixed(1)},${p[1].toFixed(1)}`).join(' ');
      faces.push(`<polygon points="${pts}" fill="${C.pcbEdge}" stroke="${C.pcbEdge}" stroke-width="0.6"/>`);
    }
    const shadow = OUTLINE.map(([x, y]) => this.project(x + 1.2, y + 1.6, -0.2)).map((p) => `${p[0].toFixed(1)},${p[1].toFixed(1)}`).join(' ');
    return `<polygon class="shadow" points="${shadow}"/>${faces.join('')}`;
  }

  drawSurface() {
    const g = [];
    const outline = OUTLINE.map(([x, y]) => `${x},${y}`).join(' ');
    g.push(`<polygon points="${outline}" fill="${C.pcb}"/>`);
    // Fine copper pour hint and silkscreen edge.
    g.push(`<polygon points="${outline}" fill="none" stroke="#0fa3a8" stroke-width="0.35" transform="translate(0.6 0.6) scale(0.982)"/>`);
    for (const [x, y] of HOLES) {
      g.push(`<circle cx="${x}" cy="${y}" r="2.6" fill="#d8c27a"/><circle cx="${x}" cy="${y}" r="1.6" fill="#0a1418"/>`);
    }
    // Header silkscreen labels.
    DIGITAL.forEach((t, i) => g.push(`<text class="silk" x="${digitalX(i)}" y="5.6" font-size="0.95" text-anchor="middle">${t}</text>`));
    POWER.forEach((t, i) => g.push(`<text class="silk" x="${powerX(i)}" y="48.4" font-size="0.95" text-anchor="middle">${t}</text>`));
    g.push(`<text class="silk" x="20" y="9.2" font-size="1.05">DIGITAL (PWM ~)</text>`);
    g.push(`<text class="silk" x="44.2" y="46.2" font-size="1.05">POWER · ANALOG IN</text>`);
    g.push(`<text class="silk board-logo" x="34.2" y="45.2" font-size="2.6">UNO R4 WiFi</text>`);
    g.push(`<text class="silk" x="34.4" y="43.3" font-size="1.1">BL616CL BRIDGE</text>`);
    // ESP header J6 pads (unpopulated) and JOFF pads.
    for (const px of [16.26, 18.8, 21.34]) {
      for (const py of [10.11, 12.65]) g.push(`<circle cx="${px}" cy="${py}" r="0.85" fill="${C.copper}"/><circle cx="${px}" cy="${py}" r="0.45" fill="#0a1418"/>`);
    }
    g.push(`<text class="silk" x="15.2" y="8.6" font-size="0.9">J6 · 3V3 ONLY</text>`);
    for (const px of [17.78, 20.32, 22.86]) g.push(`<circle cx="${px}" cy="50.8" r="0.8" fill="${C.copper}"/><circle cx="${px}" cy="50.8" r="0.4" fill="#0a1418"/>`);
    // LQFP leads and module castellations.
    for (let i = 0; i < 16; i++) {
      const o = 13.1 + i * 0.5 + 0.12;
      g.push(`<rect x="46.6" y="${o}" width="1.2" height="0.25" fill="#b7bcc0"/>`);
      g.push(`<rect x="57.7" y="${o}" width="1.2" height="0.25" fill="#b7bcc0"/>`);
      g.push(`<rect x="${48.4 + i * 0.5}" y="11.35" width="0.25" height="1.2" fill="#b7bcc0"/>`);
      g.push(`<rect x="${48.4 + i * 0.5}" y="22.45" width="0.25" height="1.2" fill="#b7bcc0"/>`);
    }
    // Indicator LEDs: L, TX, RX, power.
    const leds = [[26.42, 11.18, '#f5c542', 'L'], [26.42, 16.76, '#f5c542', 'TX'], [26.42, 19.05, '#f5c542', 'RX'], [64.9, 13.79, '#5bd47a', 'ON']];
    for (const [x, y, col, t] of leds) {
      g.push(`<rect x="${x - 0.8}" y="${y - 0.4}" width="1.6" height="0.8" fill="${col}" opacity="0.85"/><text class="silk" x="${x + 1.3}" y="${y + 0.35}" font-size="0.9">${t}</text>`);
    }
    // 0-ohm links R45-R48 between U4 and RA4M1 SCI pins.
    for (const x of [15.95, 17.05, 20.35, 21.45]) g.push(`<rect x="${x - 0.25}" y="16.26" width="0.5" height="1.0" fill="#2a2e31"/>`);
    // Signal traces for the active lesson.
    if (this.showPaths) {
      for (const [name, p] of Object.entries(PATHS)) {
        const on = this.focus.paths.includes(name);
        const d = 'M' + p.pts.map((q) => q.join(',')).join(' L');
        g.push(`<path d="${d}" fill="none" stroke="${on ? p.color : '#0d7276'}" stroke-width="${on ? 0.55 : 0.32}" stroke-linejoin="round" stroke-linecap="round" ${on ? 'class="flow"' : ''}/>`);
      }
    }
    // 12 x 8 LED matrix, LED 1 at the top-left (near the digital header and USB side).
    for (let i = 0; i < 96; i++) {
      const col = i % 12;
      const row = Math.floor(i / 12);
      const x = 34.412 + 2.1 * col;
      const y = 26.81 + 2.1 * row;
      const on = this.matrix[i];
      if (on) g.push(`<circle cx="${x}" cy="${y}" r="1.55" fill="url(#ledGlow)"/>`);
      g.push(`<rect x="${x - 0.8}" y="${y - 0.4}" width="1.6" height="0.8" transform="rotate(225 ${x} ${y})" fill="${on ? '#ff4a3d' : '#d7cfbd'}" stroke="${on ? '#ffd2cc' : '#9c9483'}" stroke-width="0.12"/>`);
    }
    return `<g transform="${this.planeMatrix(PCB_Z)}">${g.join('')}</g>`;
  }

  drawParts() {
    const t = (this.theta * Math.PI) / 180;
    const s = Math.sin(t);
    const c = Math.cos(t);
    // Painter order: resolve overlapping footprints by height, separated ones by view direction.
    const parts = PARTS.slice();
    const before = (a, b) => {
      const sepX = a.x + a.w <= b.x + 1e-6 ? 1 : b.x + b.w <= a.x + 1e-6 ? -1 : 0;
      const sepY = a.y + a.d <= b.y + 1e-6 ? 1 : b.y + b.d <= a.y + 1e-6 ? -1 : 0;
      if (!sepX && !sepY) return a.z + a.h <= b.z + 1e-6 ? -1 : b.z + b.h <= a.z + 1e-6 ? 1 : 0;
      const gx = sepX ? Math.abs(s) : -1;
      const gy = sepY ? Math.abs(c) : -1;
      if (gx >= gy) return sepX * s > 0 ? -1 : 1;
      return sepY * c > 0 ? -1 : 1;
    };
    const depth = (p) => (p.x + p.w / 2 - CX) * s + (p.y + p.d / 2 - CY) * c;
    parts.sort((a, b) => depth(a) - depth(b));
    // Stable topological pass: move a part after anything that must be drawn first.
    const ordered = [];
    const pending = parts.slice();
    while (pending.length) {
      let idx = pending.findIndex((p) => pending.every((q) => q === p || before(q, p) !== -1 || !this.overlapScreen(q, p)));
      if (idx < 0) idx = 0;
      ordered.push(pending.splice(idx, 1)[0]);
    }
    return ordered.map((p) => this.drawBox(p, s, c)).join('');
  }

  bounds(p) {
    let x0 = Infinity, x1 = -Infinity, y0 = Infinity, y1 = -Infinity;
    for (const [x, y] of [[p.x, p.y], [p.x + p.w, p.y], [p.x, p.y + p.d], [p.x + p.w, p.y + p.d]]) {
      for (const z of [p.z, p.z + p.h]) {
        const q = this.project(x, y, z);
        x0 = Math.min(x0, q[0]); x1 = Math.max(x1, q[0]);
        y0 = Math.min(y0, q[1]); y1 = Math.max(y1, q[1]);
      }
    }
    return [x0, y0, x1, y1];
  }

  overlapScreen(a, b) {
    const A = this.bounds(a);
    const B = this.bounds(b);
    return A[0] < B[2] && B[0] < A[2] && A[1] < B[3] && B[1] < A[3];
  }

  drawBox(p, s, c) {
    const P = (x, y, z) => this.project(x, y, z);
    const x0 = p.x, x1 = p.x + p.w, y0 = p.y, y1 = p.y + p.d, z0 = p.z, z1 = p.z + p.h;
    const pts = (arr) => arr.map((q) => `${q[0].toFixed(1)},${q[1].toFixed(1)}`).join(' ');
    const focused = this.isFocused(p.group);
    const dim = this.focus.parts.length && !focused;
    const faces = [];
    const side = (corners, nx, ny) => {
      const facing = nx * s + ny * c;
      if (facing <= 0) return;
      const light = nx * Math.cos((this.theta * Math.PI) / 180) - ny * Math.sin((this.theta * Math.PI) / 180);
      const f = 0.62 + 0.14 * light;
      faces.push(`<polygon points="${pts(corners)}" fill="${shade(p.color, f)}"/>`);
    };
    side([P(x0, y1, z0), P(x1, y1, z0), P(x1, y1, z1), P(x0, y1, z1)], 0, 1);
    side([P(x0, y0, z0), P(x1, y0, z0), P(x1, y0, z1), P(x0, y0, z1)], 0, -1);
    side([P(x1, y0, z0), P(x1, y1, z0), P(x1, y1, z1), P(x1, y0, z1)], 1, 0);
    side([P(x0, y0, z0), P(x0, y1, z0), P(x0, y1, z1), P(x0, y0, z1)], -1, 0);
    const topFill = p.top === 'blcan' ? 'url(#canSheen)' : p.metal ? shade(p.color, 1.08) : p.color;
    faces.push(`<polygon points="${pts([P(x0, y0, z1), P(x1, y0, z1), P(x1, y1, z1), P(x0, y1, z1)])}" fill="${topFill}" stroke="${shade(p.color, 0.75)}" stroke-width="0.4"/>`);
    const decal = this.decal(p);
    if (decal) faces.push(`<g transform="${this.planeMatrix(z1)}">${decal}</g>`);
    const cls = ['part', focused ? 'focused' : '', dim ? 'dim' : ''].join(' ');
    return `<g class="${cls}" data-id="${p.group}">${faces.join('')}</g>`;
  }

  decal(p) {
    switch (p.top) {
      case 'antenna': {
        // Printed meander antenna at the module end.
        const d = 'M1.2,26.2 L5.6,26.2 L5.6,27.6 L1.2,27.6 L1.2,29 L5.6,29 L5.6,30.4 L1.2,30.4 L1.2,31.8 L5.6,31.8 L5.6,33.2 L1.2,33.2 L1.2,34.6 L5.6,34.6 L5.6,37.2';
        return `<path d="${d}" fill="none" stroke="${C.copper}" stroke-width="0.42"/><text class="chiptext" x="3.4" y="38.9" font-size="0.75" text-anchor="middle">ANT</text>`;
      }
      case 'blcan':
        return `<text class="chiptext dark" x="13.45" y="31.4" font-size="2.3" text-anchor="middle" font-weight="700">BL616CL</text><text class="chiptext dark" x="13.45" y="33.7" font-size="1.05" text-anchor="middle">BOUFFALO LAB</text><circle cx="8" cy="26" r="0.45" fill="#7f8a90"/>`;
      case 'ra':
        return `<text class="chiptext" x="52.75" y="17.4" font-size="1.7" text-anchor="middle" font-weight="700">RA4M1</text><text class="chiptext" x="52.75" y="19.3" font-size="0.85" text-anchor="middle">R7FA4M1AB3CFM</text><circle cx="49" cy="13.7" r="0.45" fill="#555b60"/>`;
      case 'u4':
        return `<text class="chiptext" x="18.7" y="20.35" font-size="0.62" text-anchor="middle">TXB0108</text>`;
      case 'ind':
        return `<text class="chiptext" x="29.51" y="36.4" font-size="1" text-anchor="middle">100</text>`;
      case 'dc':
        return `<rect x="-1.4" y="41.3" width="13.3" height="7.8" rx="1.2" fill="none" stroke="#3a4045" stroke-width="0.4"/>`;
      case 'hdr': {
        let h = '';
        for (let i = 0; i < p.pins; i++) {
          const x = p.first + 2.54 * i;
          const y = p.y + 1.27;
          h += `<rect x="${x - 0.5}" y="${y - 0.5}" width="1" height="1" fill="#050607"/>`;
        }
        return h;
      }
      default:
        return '';
    }
  }

  drawOverlay() {
    const out = [];
    // Off-board actors sit in screen space at the stage edges.
    const actorPos = {
      pc: [this.W * 0.09, this.H * 0.8],
      ap: [this.W * 0.1, this.H * 0.16],
      phone: [this.W * 0.9, this.H * 0.16],
    };
    for (const a of this.focus.actors) {
      const pos = actorPos[a.id];
      if (!pos) continue;
      const from = this.project(...ANCHORS[a.from]);
      const mid = [(from[0] + pos[0]) / 2, Math.min(from[1], pos[1]) - 30];
      const dash = a.radio ? 'stroke-dasharray="2 7"' : '';
      out.push(`<path class="link ${a.radio ? 'radio' : 'cable'}" d="M${from[0].toFixed(1)},${from[1].toFixed(1)} Q${mid[0].toFixed(1)},${mid[1].toFixed(1)} ${pos[0].toFixed(1)},${pos[1].toFixed(1)}" ${dash}/>`);
      out.push(`<g class="actor" transform="translate(${pos[0].toFixed(1)} ${pos[1].toFixed(1)})">${ACTOR_ICONS[a.id]}<text y="34" text-anchor="middle">${this.actorText[a.id] || a.id}</text></g>`);
    }
    if (this.showLabels) {
      for (const id of this.focus.labels) {
        const anchor = ANCHORS[id];
        if (!anchor) continue;
        const [x, y] = this.project(...anchor);
        const text = this.labelText[id] || id;
        const w = Math.max(44, [...text].reduce((sum, c) => sum + (c.charCodeAt(0) > 255 ? 11 : 6.3), 18));
        const offsets = { bl: [-5, 48], usb: [-20, -32], u4: [28, -60], j6: [-20, -88], ra: [20, -42], matrix: [35, 40], antenna: [-10, 34] };
        const [dx, dy] = offsets[id] || [0, -34];
        const lx = Math.max(w / 2 + 4, Math.min(this.W - w / 2 - 4, x + dx));
        const ly = Math.max(14, Math.min(this.H - 14, y + dy));
        out.push(`<g class="hotspot" data-id="${id}" tabindex="0" role="button" aria-label="${text}">
          <line x1="${x.toFixed(1)}" y1="${y.toFixed(1)}" x2="${lx.toFixed(1)}" y2="${ly.toFixed(1)}"/>
          <circle class="dot" cx="${x.toFixed(1)}" cy="${y.toFixed(1)}" r="4.2"/>
          <rect x="${(lx - w / 2).toFixed(1)}" y="${(ly - 11).toFixed(1)}" width="${w.toFixed(1)}" height="22" rx="11"/>
          <text x="${lx.toFixed(1)}" y="${(ly + 4).toFixed(1)}" text-anchor="middle">${text}</text>
        </g>`);
      }
    }
    return out.join('');
  }

  // Point at fraction u along a path, projected to the screen.
  pathPoint(name, u) {
    const pts = PATHS[name].pts;
    const segs = [];
    let total = 0;
    for (let i = 1; i < pts.length; i++) {
      const L = Math.hypot(pts[i][0] - pts[i - 1][0], pts[i][1] - pts[i - 1][1]);
      segs.push(L);
      total += L;
    }
    let d = Math.max(0, Math.min(1, u)) * total;
    for (let i = 0; i < segs.length; i++) {
      if (d <= segs[i] || i === segs.length - 1) {
        const f = segs[i] ? d / segs[i] : 0;
        const x = pts[i][0] + (pts[i + 1][0] - pts[i][0]) * f;
        const y = pts[i][1] + (pts[i + 1][1] - pts[i][1]) * f;
        return this.project(x, y, PCB_Z + 0.2);
      }
      d -= segs[i];
    }
    return this.project(pts[0][0], pts[0][1], PCB_Z);
  }

  // Animate a glowing packet along a board path or an off-board link.
  packet({ path, reverse = false, color = '#fff', actor = null, toActor = true }) {
    return new Promise((resolve) => {
      const dur = matchMedia('(prefers-reduced-motion: reduce)').matches ? 1 : 900;
      const fx = { t0: performance.now(), dur, path, reverse, color, actor, toActor, resolve };
      this.fx.push(fx);
      if (this.fx.length === 1) requestAnimationFrame(() => this.tick());
    });
  }

  actorScreen(id) {
    return { pc: [this.W * 0.09, this.H * 0.8], ap: [this.W * 0.1, this.H * 0.16], phone: [this.W * 0.9, this.H * 0.16] }[id];
  }

  tick() {
    const now = performance.now();
    this.fx = this.fx.filter((f) => {
      if (now - f.t0 >= f.dur) {
        f.resolve();
        return false;
      }
      return true;
    });
    this.drawFx(now);
    if (this.fx.length) requestAnimationFrame(() => this.tick());
  }

  drawFx(now = performance.now()) {
    if (!this.fxLayer) return;
    const out = [];
    for (const f of this.fx) {
      let u = (now - f.t0) / f.dur;
      if (f.reverse) u = 1 - u;
      let p;
      if (f.actor) {
        const a = this.actorScreen(f.actor.id);
        const b = this.project(...ANCHORS[f.actor.from]);
        const v = f.toActor ? (now - f.t0) / f.dur : 1 - (now - f.t0) / f.dur;
        const mid = [(a[0] + b[0]) / 2, Math.min(a[1], b[1]) - 30];
        const w = Math.max(0, Math.min(1, v));
        p = [
          (1 - w) * (1 - w) * b[0] + 2 * (1 - w) * w * mid[0] + w * w * a[0],
          (1 - w) * (1 - w) * b[1] + 2 * (1 - w) * w * mid[1] + w * w * a[1],
        ];
      } else {
        p = this.pathPoint(f.path, u);
      }
      out.push(`<circle cx="${p[0].toFixed(1)}" cy="${p[1].toFixed(1)}" r="11" fill="url(#pktGlow)" opacity="0.8"/><circle cx="${p[0].toFixed(1)}" cy="${p[1].toFixed(1)}" r="4" fill="${f.color}" stroke="#fff" stroke-width="1.5"/>`);
    }
    this.fxLayer.innerHTML = out.join('');
  }
}

const ACTOR_ICONS = {
  pc: '<rect x="-22" y="-16" width="44" height="28" rx="3" class="ic"/><rect x="-18" y="-12" width="36" height="20" rx="1.5" class="ic-screen"/><path d="M-28 15 h56 l-4 5 h-48z" class="ic"/>',
  ap: '<rect x="-20" y="2" width="40" height="13" rx="3" class="ic"/><path d="M-12 2 v-14 M12 2 v-14" class="ic-line"/><circle cx="-12" cy="9" r="1.8" class="ic-led"/><circle cx="-5" cy="9" r="1.8" class="ic-led"/><path d="M-7 -16 a10 10 0 0 1 14 0 M-12 -21 a17 17 0 0 1 24 0" class="ic-line"/>',
  phone: '<rect x="-12" y="-20" width="24" height="40" rx="5" class="ic"/><rect x="-9" y="-15" width="18" height="29" rx="1.5" class="ic-screen"/><circle cx="0" cy="17" r="1.5" class="ic-led"/>',
};
