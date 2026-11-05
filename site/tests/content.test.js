import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { LESSONS, STR, PRESETS, PRESET_STATUS } from '../assets/content.js';
import { PATHS } from '../assets/board.js';
import { packFrame, escapeHtml, visibleBytes } from '../assets/protocol.js';

const ino = await readFile(new URL('../../examples/PhoneMatrix/PhoneMatrix.ino', import.meta.url), 'utf8');

test('every lesson has matching Chinese and English content and a unique deep link', () => {
  assert.equal(new Set(LESSONS.map((l) => l.id)).size, LESSONS.length);
  assert.equal(LESSONS.length, 14);
  for (const lesson of LESSONS) {
    for (const lang of ['zh', 'en']) {
      assert.ok(lesson[lang].title);
      assert.ok(lesson[lang].lead);
      assert.ok(lesson[lang].points.length);
    }
    assert.equal(lesson.zh.points.length, lesson.en.points.length);
    for (const path of lesson.focus.paths) assert.ok(PATHS[path]);
  }
  assert.deepEqual(Object.keys(STR.zh).sort(), Object.keys(STR.en).sort());
});

test('PhoneMatrix presets and response names agree with the actual RA4M1 example', () => {
  for (const [id, name] of [['01', 'Heart'], ['02', 'Smile'], ['03', 'Checker']]) {
    const declaration = ino.match(new RegExp(`k${name}\\[kRows\\] = \\{([^}]+)\\}`))[1];
    const bits = [...declaration.matchAll(/"([01]{12})"/g)].map((m) => m[1]).join('').split('').map(Number);
    assert.deepEqual(PRESETS[id], bits);
    assert.ok(ino.includes(PRESET_STATUS[id]));
  }
  assert.equal(PRESETS['00'].length, 96);
  assert.equal(PRESETS['00'].reduce((a, b) => a + b), 0);
});

test('raw frame preserves top-left and bottom-right ordering from the phone contract', () => {
  const bits = new Array(96).fill(0);
  bits[0] = bits[95] = 1;
  assert.equal(packFrame(bits), '10 80 00 00 00 00 00 00 00 00 00 00 01');
  assert.equal(packFrame(new Array(96).fill(1)), '10 FF FF FF FF FF FF FF FF FF FF FF FF');
  assert.equal(packFrame(PRESETS['00']), '10 00 00 00 00 00 00 00 00 00 00 00 00');
});

test('HTML escape and line-ending display preserve raw protocol notation', () => {
  assert.equal(escapeHtml('<04 0E> & "x"'), '&lt;04 0E&gt; &amp; &quot;x&quot;');
  assert.equal(visibleBytes('+FWVERSION: 0.1.0\r\nOK\r\n'), '+FWVERSION: 0.1.0\\r\\nOK\\r\\n');
});

test('declared send lengths equal the displayed payload lengths', () => {
  for (const lesson of LESSONS) {
    for (const [, , text] of lesson.replay || []) {
      if (typeof text !== 'string') continue;
      const match = text.match(/^AT\+(?:SSLCLIENTSEND|CLIENTSEND|UDPWRITE)=\d+,(\d+)\r\n(.*)$/s);
      if (match) assert.equal(Buffer.byteLength(match[2]), Number(match[1]), lesson.id);
    }
  }
});

test('candidate publication and security boundaries remain explicit in both languages', () => {
  for (const lang of ['zh', 'en']) {
    const support = LESSONS.find((l) => l.id === 'support')[lang];
    assert.match(support.lead, lang === 'zh' ? /尚未正式发行/ : /has not been released/);
    assert.match(support.lead, /1V0/);
    const tls = LESSONS.find((l) => l.id === 'tls')[lang];
    assert.match(tls.points.join(' '), lang === 'zh' ? /不检查证书有效期/ : /validity dates are not checked/);
  }
  for (const l of LESSONS.filter((l) => l.replay || l.interactive)) assert.equal(l.status, 'experimental');
});

test('lesson hash does not collide with a page element and local assets use relative URLs', async () => {
  const html = await readFile(new URL('../index.html', import.meta.url), 'utf8');
  const ids = [...html.matchAll(/\bid="([^"]+)"/g)].map((m) => m[1]);
  for (const l of LESSONS) assert.ok(!ids.includes(l.id), l.id);
  const assets = [...html.matchAll(/(?:href|src)="(\.\/assets\/[^"?#]+)"/g)].map((m) => m[1]);
  assert.ok(assets.length >= 3);
  for (const path of assets) await readFile(new URL(`../${path}`, import.meta.url));
});
