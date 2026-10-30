// sha512-crypt ($6$), as crypt(3) and /etc/shadow have it, so that the page can hand the device a password's hash
// and never the password: the installer's `password` command takes "$6$<salt>$<hash>" and the device writes it to
// the system's /etc/shadow. The algorithm is Ulrich Drepper's "Unix crypt using SHA-256 and SHA-512" (2007), with
// its default of 5000 rounds (no "rounds=" in the output). The digests come from WebCrypto, which is why every
// function here is async: 5000 rounds are 5000 digests in a row, well under a second.
//
//   await sha512crypt("Hello world!", "saltstring") === "$6$saltstring$svn8UoSVapNtMuq1ukKS4tPQd8iKwSMHWjl/O817G3uBnIFNjnQJuesI68u4OTLiBFdcbYEdFCoEOfaS35inz1" (Drepper's vector)

const ROUNDS = 5000;
const SALT_CHARS = "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
const SALT_MAX = 16;
// the bytes of the final digest in the order crypt(3) writes them, three at a time (the last alone)
const ORDER = [42, 21, 0, 1, 43, 22, 23, 2, 44, 45, 24, 3, 4, 46, 25, 26, 5, 47, 48, 27, 6, 7, 49, 28, 29, 8, 50, 51, 30, 9,
  10, 52, 31, 32, 11, 53, 54, 33, 12, 13, 55, 34, 35, 14, 56, 57, 36, 15, 16, 58, 37, 38, 17, 59, 60, 39, 18, 19, 61, 40, 41, 20, 62, 63];

const subtle = (globalThis.crypto || {}).subtle;

async function sha512(...parts) {
  const len = parts.reduce((n, p) => n + p.length, 0);
  const buf = new Uint8Array(len);
  let o = 0;
  for (const p of parts) { buf.set(p, o); o += p.length; }
  return new Uint8Array(await subtle.digest("SHA-512", buf));
}

// bytes repeated to exactly n bytes
function fill(src, n) {
  const out = new Uint8Array(n);
  for (let o = 0; o < n; o += src.length) out.set(src.subarray(0, Math.min(src.length, n - o)), o);
  return out;
}

// crypt(3)'s base64: 6 bits at a time from the low end of each 3-byte group, with its own alphabet
function encode(bytes) {
  let out = "";
  for (let i = 0; i < bytes.length; i += 3) {
    const n = Math.min(3, bytes.length - i);
    let w = 0;
    for (let k = 0; k < n; k++) w |= bytes[i + k] << (8 * k);
    for (let k = 0; k < n + 1; k++) { out += SALT_CHARS[w & 0x3f]; w >>>= 6; }
  }
  return out;
}

// a fresh salt of 16 characters from the browser's random numbers
export function makeSalt() {
  const r = new Uint8Array(SALT_MAX);
  globalThis.crypto.getRandomValues(r);
  let s = "";
  for (const b of r) s += SALT_CHARS[b & 0x3f];
  return s;
}

export const isSalt = (s) => typeof s === "string" && s.length >= 1 && s.length <= SALT_MAX && [...s].every((c) => SALT_CHARS.includes(c));
export const isHash = (h) => /^\$6\$[./0-9A-Za-z]{1,16}\$[./0-9A-Za-z]{86}$/.test(String(h));

// sha512crypt(password, salt?) -> "$6$salt$hash"; the salt is made here when not given
export async function sha512crypt(password, salt) {
  if (!subtle) throw new Error("WebCrypto is not available");
  if (salt === undefined) salt = makeSalt();
  if (!isSalt(salt)) throw new Error("bad salt");
  const enc = new TextEncoder();
  const key = enc.encode(String(password)), saltBytes = enc.encode(salt);
  const klen = key.length;

  // B: key, salt, key
  const B = await sha512(key, saltBytes, key);
  // A: key, salt, then B for every byte of the key, then for every bit of the key's length B (1) or the key (0)
  const parts = [key, saltBytes, fill(B, klen)];
  for (let n = klen; n > 0; n >>= 1) parts.push(n & 1 ? B : key);
  let A = await sha512(...parts);
  // P: the digest of the key repeated as many times as it has bytes, cut to the key's length
  const DP = await sha512(fill(key, klen * klen));
  const P = fill(DP, klen);
  // S: the digest of the salt repeated 16 + A[0] times, cut to the salt's length
  const DS = await sha512(fill(saltBytes, saltBytes.length * (16 + A[0])));
  const S = fill(DS, saltBytes.length);

  for (let i = 0; i < ROUNDS; i++) {
    const r = [];
    r.push(i & 1 ? P : A);
    if (i % 3) r.push(S);
    if (i % 7) r.push(P);
    r.push(i & 1 ? A : P);
    A = await sha512(...r);
  }
  const ordered = new Uint8Array(ORDER.length);
  ORDER.forEach((from, to) => { ordered[to] = A[from]; });
  return `$6$${salt}$${encode(ordered)}`;
}
