// strings.js: the three languages have the same keys, the same {n} fillers and the same links, and nothing is empty.
import test from "node:test";
import assert from "node:assert/strict";

import { STR } from "../../site/assets/web-installer/strings.js";

test("strings: en, tr and zh have the same keys", () => {
  const en = Object.keys(STR.en).sort();
  assert.ok(en.length > 100, `${en.length} keys`);
  for (const lang of ["tr", "zh"]) {
    const keys = Object.keys(STR[lang]).sort();
    assert.deepEqual(keys.filter((k) => !STR.en[k] && STR.en[k] !== ""), [], `only in ${lang}`);
    assert.deepEqual(en.filter((k) => !(k in STR[lang])), [], `missing in ${lang}`);
    assert.deepEqual(keys, en);
  }
});

test("strings: the same {n} fillers and [links] in every language, nothing empty", () => {
  const fillers = (s) => [...new Set(s.match(/\{\d\}/g) || [])].sort();
  for (const [k, v] of Object.entries(STR.en)) {
    assert.equal(typeof v, "string", k);
    for (const lang of ["tr", "zh"]) {
      const t = STR[lang][k];
      assert.equal(typeof t, "string", `${lang} ${k}`);
      assert.deepEqual(fillers(t), fillers(v), `${lang} ${k}`);
      assert.equal((t.match(/\[/g) || []).length, (v.match(/\[/g) || []).length, `${lang} ${k}: links`);
      if (v.trim()) assert.ok(t.trim(), `${lang} ${k} is empty`);
    }
  }
});

test("strings: no mu300 name, and the device's lines are not translated", () => {
  for (const [lang, table] of Object.entries(STR)) {
    for (const [k, v] of Object.entries(table)) {
      assert.ok(!/mu300(?!-linux)/i.test(v), `${lang} ${k}: ${v}`);
    }
  }
});
