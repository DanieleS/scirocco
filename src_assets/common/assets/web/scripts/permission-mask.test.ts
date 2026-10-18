import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import test from 'node:test';

// The web UIs mask client permissions with a hardcoded copy of crypto::PERM::_all.
// When a bit is added to crypto.h and the copy is not updated, the UI silently
// strips that permission on load and on save.
function backendPermissions(): Record<string, number> {
  const header = readFileSync(new URL('../../../../../src/crypto.h', import.meta.url), 'utf8');
  const body = /enum class PERM : uint32_t \{([\s\S]*?)\};/.exec(header)?.[1];
  assert.ok(body, 'PERM enum not found in src/crypto.h');
  const values: Record<string, number> = {};
  for (const line of body.split('\n')) {
    const match = /^\s*(\w+)\s*=\s*([^,]+),/.exec(line.replace(/\/\/.*$/, ''));
    if (!match) continue;
    const [, name, expression] = match;
    values[name] = expression.split('|').reduce((mask, term) => {
      const [base, shift] = term.split('<<').map((part) => part.trim());
      const value = /^\d+$/.test(base) ? Number(base) : values[base];
      assert.ok(value !== undefined, `unknown PERM operand ${base}`);
      return (mask | (shift === undefined ? value : value << Number(shift))) >>> 0;
    }, 0);
  }
  return values;
}

const perm = backendPermissions();

test('web UI permission masks match crypto::PERM::_all', () => {
  const all = perm._all;
  assert.ok(all & perm.telemetry_read && all & perm.unlock);
  const sources: Array<[string, RegExp]> = [
    ['../views/DevicesView.vue', /const PERMISSION_ALL = (0x[0-9a-f]+);/i],
    ['../pin.html', /\b_all: (0x[0-9a-f]+),/i],
    ['../../web-legacy/pin.html', /\b_all: (0x[0-9a-f]+),/i],
    ['../../web-legacy/views/ClientManagementView.vue', /\b_all: (0x[0-9a-f]+),/i],
  ];
  for (const [path, pattern] of sources) {
    const literal = pattern.exec(readFileSync(new URL(path, import.meta.url), 'utf8'))?.[1];
    assert.ok(literal, `${path}: permission mask not found`);
    assert.equal(
      Number(literal),
      all,
      `${path}: mask ${literal} != PERM::_all 0x${all.toString(16)}`,
    );
  }
});

test('client editor permission toggles match crypto::PERM bits', () => {
  const editor = readFileSync(
    new URL('../components/devices/ClientSettingsEditor.vue', import.meta.url),
    'utf8',
  );
  const options = [...editor.matchAll(/\{ key: '(\w+)', mask: (0x[0-9a-f]+) \}/gi)];
  assert.ok(options.length > 0);
  for (const [, key, mask] of options) {
    assert.equal(Number(mask), perm[key], `ClientSettingsEditor ${key}`);
  }
});
