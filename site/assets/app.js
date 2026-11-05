import { Board, PATHS } from './board.js';
import { STR, LABELS, PART_INFO, LESSONS, PRESETS, PRESET_STATUS, REPO } from './content.js';
import { escapeHtml, packFrame, visibleBytes } from './protocol.js';

const $ = (s) => document.querySelector(s);
const $$ = (s) => [...document.querySelectorAll(s)];
const tr = (value) => typeof value === 'string' ? value : value[lang];
const md = (text) => escapeHtml(text).replace(/`([^`]+)`/g, '<code>$1</code>').replace(/\*\*([^*]+)\*\*/g, '<strong>$1</strong>');
const icons = {
  board: '<rect x="3" y="5" width="18" height="14" rx="2"/><rect x="7" y="8" width="5" height="6"/><path d="M15 9h3m-3 4h3M7 3v2m4-2v2m4-2v2m4-2v2M7 19v2m4-2v2m4-2v2m4-2v2"/>',
  usb: '<path d="M12 20V4m-3 3 3-3 3 3M12 15l-5-4V8m5 4 5-3V7"/><circle cx="7" cy="7" r="1"/><path d="M16 4h2v2h-2z"/>',
  upload: '<path d="M12 16V3m-4 4 4-4 4 4M4 15v5h16v-5"/>',
  at: '<path d="M5 6l5 6-5 6m9 0h6"/>',
  wifi: '<path d="M2 8a15 15 0 0 1 20 0M5 12a10 10 0 0 1 14 0m-11 4a5 5 0 0 1 8 0"/><circle cx="12" cy="20" r="1"/>',
  ap: '<rect x="3" y="14" width="18" height="6" rx="1"/><path d="M6 14V7m12 7V7m-9 11h0m4 0h0M9 6a4 4 0 0 1 6 0M6 3a9 9 0 0 1 12 0"/>',
  lock: '<rect x="5" y="10" width="14" height="11" rx="2"/><path d="M8 10V7a4 4 0 0 1 8 0v3m-4 4v3"/>',
  udp: '<path d="M3 7h16m-4-4 4 4-4 4M21 17H5m4-4-4 4 4 4"/>',
  clock: '<circle cx="12" cy="12" r="9"/><path d="M12 6v6l4 3"/>',
  file: '<path d="M5 3h9l5 5v13H5zM14 3v6h5M8 13h8m-8 4h6"/>',
  ble: '<path d="m7 6 10 11-5 4V3l5 4L7 18"/>',
  matrix: '<rect x="3" y="5" width="18" height="14" rx="2"/><path d="M7 9h0m5 0h0m5 0h0M7 15h0m5 0h0m5 0h0" stroke-width="3"/>',
  bug: '<rect x="8" y="6" width="8" height="14" rx="4"/><path d="m9 7-3-4m9 4 3-4M8 10H3m13 0h5M8 15H3m13 0h5M12 6v14"/>',
  check: '<path d="M12 2 3 6v7c0 5 9 9 9 9s9-4 9-9V6zM8 12l3 3 5-6"/>',
  left: '<path d="M5 10a8 8 0 1 1-1 7M5 4v6h6"/>',
  right: '<path d="M19 10a8 8 0 1 0 1 7m-1-13v6h-6"/>',
  reset: '<path d="M4 10V4h6m10 10v6h-6M4 4l6 6m10 10-6-6"/>',
  play: '<path d="m7 4 13 8-13 8z"/>',
  stop: '<path d="M5 5h14v14H5z"/>',
};
const icon = (name) => `<svg viewBox="0 0 24 24" aria-hidden="true">${icons[name] || icons.board}</svg>`;

let lang = new URL(location.href).searchParams.get('lang') === 'en' ? 'en' : 'zh';
let current = 0;
let tab = 'learn';
let replayToken = 0;
let playing = false;
let matrix = PRESETS['01'].slice();
let preset = '01';
let seen;
try { seen = new Set(JSON.parse(localStorage.getItem('bridge-viewed') || '[]')); }
catch { seen = new Set(); }
const board = new Board($('#board-svg'), { onHotspot: showPart });

function strings() {
  const s = STR[lang];
  document.documentElement.lang = lang === 'zh' ? 'zh-CN' : 'en';
  document.title = `${s.title} · Bridge Explorer`;
  $('#tagline').textContent = s.tagline;
  $('#support-link').textContent = s.navSupport;
  $('#repo-link').textContent = `${s.navRepo} ↗`;
  $('#language').textContent = s.lang;
  $('#language').title = s.langTitle;
  $('#language').setAttribute('aria-label', s.langTitle);
  $('#release-note').textContent = s.release;
  $('#stage-hint').textContent = s.hint;
  $('#curriculum-title').textContent = s.navLessons;
  $('#footer-text').textContent = s.footer;
  $('#credit').textContent = s.credit;
  $('#trademark').textContent = s.trademark;
  $('#license').textContent = s.license;
  $('#tab-learn').textContent = s.tabLearn;
  $('#tab-code').textContent = s.tabCode;
  $('#tab-pins').textContent = s.tabPins;
  $('#copy-code').textContent = s.copy;
  $('#close-part').setAttribute('aria-label', s.close);
  $('#matrix-title').textContent = s.matrixPresets;
  $('#matrix-sim-label').textContent = s.replayNote;
  $('#matrix-hint').textContent = s.matrixHint;
  $('#matrix-grid').setAttribute('aria-label', lang === 'zh' ? '12 列 8 行点阵编辑器' : '12-column, 8-row matrix editor');
  $('#payload-label').textContent = s.matrixPayload;
  $('#matrix-send').textContent = s.matrixSend;
  $('#drawing-note').textContent = lang === 'zh' ? '交互示意图 · 非精确 CAD · 1V0 模组' : 'Illustration · not exact CAD · 1V0 module';
  $('#board-svg').setAttribute('aria-label', lang === 'zh' ? 'BL616CL 模组替换后的 UNO R4 WiFi 等轴测交互示意图' : 'Interactive isometric UNO R4 WiFi illustration with a BL616CL replacement module');
  const controlMap = { left: 'rotateL', right: 'rotateR', out: 'zoomOut', in: 'zoomIn', reset: 'resetView', labels: 'labels', paths: 'paths' };
  $$('[data-board]').forEach((b) => {
    const name = b.dataset.board;
    b.title = s[controlMap[name]];
    b.setAttribute('aria-label', b.title);
    if (['left', 'right', 'reset'].includes(name)) b.innerHTML = icon(name);
    else if (['labels', 'paths'].includes(name)) b.textContent = s[name];
  });
  $('#pin-note').textContent = lang === 'zh' ? '仅适用于已验证的 BL616CL 模组 1V0。U4 在 RA4M1 的 5 V 和模组的 3.3 V 之间转换。彩色线表示逻辑通路，不是 PCB 实际走线。' : 'For the validated BL616CL module 1V0 only. U4 shifts between the RA4M1 at 5 V and the module at 3.3 V. Colored lines show logical paths, not actual PCB tracks.';
  board.setText(LABELS[lang], s.actors);
}

function render() {
  const lesson = LESSONS[current];
  const c = lesson[lang];
  const s = STR[lang];
  $('#part-popover').hidden = true;
  $('#stage-title').textContent = c.title;
  $('#lesson-title').textContent = c.title;
  $('#lesson-number').textContent = `${s.lesson.replace('{n}', String(current + 1).padStart(2, '0'))} / ${LESSONS.length}`;
  $('#lesson-status').innerHTML = lesson.status ? `<span class="badge ${lesson.status}">${s.status[lesson.status]}</span>` : '';
  $('#lesson-lead').innerHTML = md(c.lead);
  $('#lesson-points').innerHTML = c.points.map((p) => `<li>${md(p)}</li>`).join('');
  $('#support-table').innerHTML = lesson.table ? `<table><caption class="eyebrow">${lang === 'zh' ? '状态与实机证据范围（历史版本）' : 'Status and hardware evidence scope (historical build)'}</caption><tbody>${lesson.table.map(([status, text, tested]) => `<tr><td><span class="badge ${status}">${s.status[status]}</span>${tested ? `<small>◌ ${s.tested}</small>` : ''}</td><td>${escapeHtml(tr(text))}</td></tr>`).join('')}</tbody></table>` : '';
  $('#lesson-links').innerHTML = (lesson.links || sourceLinks(lesson.id)).map(([name, href]) => `<a href="${href}">${name} ↗</a>`).join('');
  $('#code').textContent = c.code?.text || s.noCode;
  $('#code-lang').textContent = c.code ? `${c.code.lang.toUpperCase()} · ${lang === 'zh' ? '代码片段（非完整工程）' : 'snippet (not a complete project)'}` : '';
  $('#copy-code').hidden = !c.code;
  $('#code-source').innerHTML = sourceLinks(lesson.id).map(([name, href]) => `<a href="${href}">${name} ↗</a>`).join('');
  $('#pin-table').innerHTML = lesson.pins ? `<thead><tr>${s.pinsHead.map((t) => `<th scope="col">${t}</th>`).join('')}</tr></thead><tbody>${lesson.pins.map((r) => `<tr>${r.map((v) => `<td>${escapeHtml(v)}</td>`).join('')}</tr>`).join('')}</tbody>` : `<tbody><tr><td>${s.noPins}</td></tr></tbody>`;
  $('#legend').innerHTML = lesson.focus.paths.map((name) => `<span><i style="background:${PATHS[name].color}"></i>${s.legend[name]}</span>`).join('');
  board.setFocus(lesson.focus);
  board.setMatrix(lesson.interactive ? matrix : PRESETS['00']);
  $('#matrix-editor').hidden = !lesson.interactive;
  $('#replay').hidden = !lesson.replay && !lesson.interactive;
  $('#replay-heading').textContent = s.replayTitle;
  $('#replay-note').textContent = s.replayNote;
  $('#terminal').innerHTML = `<span class="terminal-empty">${lang === 'zh' ? '点播放，沿着板上的通路看一次数据往返。\\r\\n 表示行尾；尖括号表示示意字节。' : 'Press Play to follow a round trip across the board. \\r\\n marks line endings; angle brackets stand for illustrative bytes.'}</span>`;
  setPlaying(false);
  $('#prev').textContent = `← ${s.prev}`;
  $('#next').textContent = `${s.next} →`;
  $('#prev').disabled = current === 0;
  $('#next').disabled = current === LESSONS.length - 1;
  $('#lesson-count').textContent = `${String(current + 1).padStart(2, '0')} / ${LESSONS.length}`;
  renderCards();
  if (lesson.interactive) renderMatrix();
  selectTab(tab);
  $('.lesson-scroll').scrollTop = 0;
}

function sourceLinks(id) {
  if (id === 'matrix') return [['PhoneMatrix.ino', `${REPO}/blob/master/examples/PhoneMatrix/PhoneMatrix.ino`], ['Example guide', `${REPO}/blob/master/examples/README.md`]];
  if (id === 'dap') return [['ra4m1_swd.cfg', `${REPO}/blob/master/tools/ra4m1_swd.cfg`]];
  if (['board', 'serial', 'upload'].includes(id)) return [['DEVELOP.md', `${REPO}/blob/master/DEVELOP.md`]];
  return [['PROTOCOL.md', `${REPO}/blob/master/docs/PROTOCOL.md`], ['at_server.cpp', `${REPO}/blob/master/UNOR4USBBridgeBL/at_server.cpp`]];
}

function renderCards() {
  const s = STR[lang];
  $('#lessons').innerHTML = LESSONS.map((l, i) => `<a class="lesson-card" href="#${l.id}" ${i === current ? 'aria-current="step"' : ''}><span class="lesson-icon">${icon(l.icon)}</span><span><small>${s.lesson.replace('{n}', String(i + 1).padStart(2, '0'))}</small><strong>${escapeHtml(l[lang].title)}</strong></span>${seen.has(l.id) ? '<span class="viewed" aria-hidden="true">✓</span>' : ''}</a>`).join('');
  $('#progress').textContent = s.progress.replace('{done}', LESSONS.filter((l) => seen.has(l.id)).length).replace('{total}', LESSONS.length);
}

function goFromHash() {
  const index = LESSONS.findIndex((l) => l.id === location.hash.slice(1));
  current = index < 0 ? 0 : index;
  stopReplay();
  seen.add(LESSONS[current].id);
  try { localStorage.setItem('bridge-viewed', JSON.stringify([...seen])); } catch { /* Storage may be disabled. */ }
  render();
  const card = $('#lessons [aria-current]');
  if (card) $('#lessons').scrollTo({ left: Math.max(0, card.offsetLeft - $('#lessons').offsetLeft - 12), behavior: 'auto' });
}

function selectTab(name) {
  tab = name;
  $$('[data-tab]').forEach((b) => {
    const active = b.dataset.tab === name;
    b.setAttribute('aria-selected', active);
    b.tabIndex = active ? 0 : -1;
  });
  ['learn', 'code', 'pins'].forEach((t) => { $(`#pane-${t}`).hidden = t !== name; });
}

function showPart(id) {
  $('#part-title').textContent = LABELS[lang][id];
  $('#part-text').textContent = PART_INFO[lang][id];
  $('#part-popover').hidden = false;
}

function setPlaying(value) {
  playing = value;
  $('#play').innerHTML = `${icon(value ? 'stop' : 'play')} ${STR[lang][value ? 'stop' : 'play']}`;
  $('#matrix-send').disabled = value;
}

function stopReplay() {
  replayToken++;
  setPlaying(false);
}

async function animateStep(from, to, path) {
  if (from === 'note') return;
  const actorId = ['pc', 'phone', 'ap'].find((id) => id === from || id === to);
  if (actorId) {
    await board.packet({ actor: { id: actorId, from: actorId === 'pc' ? 'usb' : 'antenna' }, toActor: to === actorId, color: '#fff2ab' });
  } else if (path) {
    await board.packet({ path, reverse: to === 'bl', color: PATHS[path].color });
  }
}

function addLog(from, to, text) {
  const node = document.createElement('div');
  node.className = 'log-row';
  const route = from === 'note' ? '·' : `${STR[lang].nodes[from]} → ${STR[lang].nodes[to]}`;
  node.innerHTML = `<span class="log-route">${route}</span><span class="log-msg ${from === 'note' ? 'log-note' : ''}">${escapeHtml(visibleBytes(tr(text)))}</span>`;
  $('#terminal').append(node);
  $('#terminal').scrollTop = $('#terminal').scrollHeight;
}

async function replay(steps) {
  stopReplay();
  const token = replayToken;
  setPlaying(true);
  $('#terminal').replaceChildren();
  for (const [from, to, text, path] of steps) {
    if (token !== replayToken) return;
    addLog(from, to, text);
    await animateStep(from, to, path);
    if (token !== replayToken) return;
    if (!matchMedia('(prefers-reduced-motion: reduce)').matches) await new Promise((r) => setTimeout(r, from === 'note' ? 600 : 220));
  }
  if (token === replayToken) setPlaying(false);
}

function renderMatrix() {
  $('#presets').innerHTML = ['00', '01', '02', '03'].map((p) => `<button type="button" data-preset="${p}" aria-pressed="${preset === p}">${STR[lang].presetNames[p]} <span class="muted">${p}</span></button>`).join('');
  $('#matrix-grid').innerHTML = matrix.map((on, i) => `<button type="button" data-pixel="${i}" aria-pressed="${!!on}" aria-label="${lang === 'zh' ? '行' : 'Row'} ${Math.floor(i / 12) + 1}, ${lang === 'zh' ? '列' : 'column'} ${i % 12 + 1}"></button>`).join('');
  updatePayload();
}

function updatePayload() {
  $('#matrix-payload').textContent = preset === '10' ? packFrame(matrix) : preset;
  board.setMatrix(matrix);
}

function matrixReplay() {
  const payload = preset === '10' ? packFrame(matrix) : preset;
  return [
    ['phone', 'bl', `GATT write 19B10001: <${payload}>`],
    ['bl', 'ra', { zh: 'HCI ACL → ArduinoBLE Host（经 HCIREAD）', en: 'HCI ACL → ArduinoBLE Host (via HCIREAD)' }, 'at'],
    ['ra', 'matrix', { zh: 'RA4M1 解析命令并更新 96 个像素', en: 'RA4M1 decodes the command and updates 96 pixels' }, 'matrix'],
    ['ra', 'bl', `GATT notify: ${PRESET_STATUS[preset]} (HCIWRITE)`, 'at'],
    ['bl', 'phone', `19B10002: ${PRESET_STATUS[preset]}`],
  ];
}

$('#language').addEventListener('click', () => {
  stopReplay();
  lang = lang === 'zh' ? 'en' : 'zh';
  const url = new URL(location.href);
  url.searchParams.set('lang', lang);
  history.replaceState(null, '', url);
  strings();
  render();
});
$$('[data-tab]').forEach((b) => b.addEventListener('click', () => selectTab(b.dataset.tab)));
$('.tabs').addEventListener('keydown', (e) => {
  const tabs = $$('[data-tab]');
  const index = tabs.indexOf(document.activeElement);
  if (index < 0) return;
  const next = e.key === 'ArrowRight' ? (index + 1) % 3 : e.key === 'ArrowLeft' ? (index + 2) % 3 : e.key === 'Home' ? 0 : e.key === 'End' ? 2 : -1;
  if (next >= 0) {
    e.preventDefault();
    selectTab(tabs[next].dataset.tab);
    tabs[next].focus();
  }
});
$('#copy-code').addEventListener('click', async () => {
  await navigator.clipboard.writeText(LESSONS[current][lang].code.text);
  $('#copy-code').textContent = STR[lang].copied;
});
$('#prev').addEventListener('click', () => { if (current > 0) location.hash = LESSONS[current - 1].id; });
$('#next').addEventListener('click', () => { if (current < LESSONS.length - 1) location.hash = LESSONS[current + 1].id; });
$('#close-part').addEventListener('click', () => { $('#part-popover').hidden = true; });
document.addEventListener('keydown', (e) => { if (e.key === 'Escape') $('#part-popover').hidden = true; });
$('#board-svg').addEventListener('keydown', (e) => {
  const hs = e.target.closest('.hotspot');
  if (hs && (e.key === 'Enter' || e.key === ' ')) { e.preventDefault(); showPart(hs.dataset.id); }
});
$$('[data-board]').forEach((b) => b.addEventListener('click', () => {
  const action = b.dataset.board;
  if (action === 'left') board.rotate(-30);
  if (action === 'right') board.rotate(30);
  if (action === 'in') board.zoom(1.15);
  if (action === 'out') board.zoom(1 / 1.15);
  if (action === 'reset') board.reset();
  if (action === 'labels' || action === 'paths') {
    const prop = action === 'labels' ? 'showLabels' : 'showPaths';
    board[prop] = !board[prop];
    b.setAttribute('aria-pressed', board[prop]);
    board.render();
  }
}));
$('#play').addEventListener('click', () => {
  if (playing) stopReplay();
  else replay(LESSONS[current].interactive ? matrixReplay() : LESSONS[current].replay || []);
});
$('#presets').addEventListener('click', (e) => {
  const p = e.target.closest('[data-preset]')?.dataset.preset;
  if (!p) return;
  preset = p;
  matrix = PRESETS[p].slice();
  renderMatrix();
});
$('#matrix-grid').addEventListener('click', (e) => {
  const b = e.target.closest('[data-pixel]');
  if (!b) return;
  const i = Number(b.dataset.pixel);
  matrix[i] = 1 - matrix[i];
  preset = '10';
  b.setAttribute('aria-pressed', !!matrix[i]);
  $$('#presets button').forEach((p) => p.setAttribute('aria-pressed', false));
  updatePayload();
});
$('#matrix-send').addEventListener('click', () => replay(matrixReplay()));
window.addEventListener('hashchange', goFromHash);
strings();
goFromHash();
