import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import test from 'node:test';

// The web UIs keep their own copies of the crypto::PERM bits for the permission toggles.
// They send the permissions as they are (the server masks them with PERM::_all); a copy that
// falls behind crypto.h shows or toggles the wrong bit.
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

test('web UI permission mappings match crypto::PERM', () => {
  assert.ok(perm._all & perm.telemetry_read && perm._all & perm.unlock);
  const sources = [
    '../pin.html',
    '../../web-legacy/pin.html',
    '../../web-legacy/views/ClientManagementView.vue',
  ];
  for (const path of sources) {
    const source = readFileSync(new URL(path, import.meta.url), 'utf8');
    const mapping = /const permissionMapping = \{([\s\S]*?)\}/.exec(source)?.[1];
    assert.ok(mapping, `${path}: permissionMapping not found`);
    const entries = [...mapping.matchAll(/^\s*(\w+): (0x[0-9a-f]+),/gim)];
    assert.ok(entries.length > 0, `${path}: permissionMapping is empty`);
    for (const [, name, literal] of entries) {
      const expected = name === '_all_operations' ? perm._all_opeiations : perm[name];
      assert.equal(Number(literal), expected, `${path}: ${name}`);
    }
  }
});

test('web UI saves send permissions unmasked', () => {
  for (const path of [
    '../views/DevicesView.vue',
    '../pin.html',
    '../../web-legacy/pin.html',
    '../../web-legacy/views/ClientManagementView.vue',
  ]) {
    const source = readFileSync(new URL(path, import.meta.url), 'utf8');
    assert.doesNotMatch(source, /perm: [\w.]+ & /, `${path}: masks permissions on save`);
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
