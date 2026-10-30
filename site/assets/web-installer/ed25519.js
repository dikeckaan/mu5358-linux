// Ed25519 signature verification, RFC 8032 5.1.7, for the release's signature (SHA256SUMS.sig). verifyEd25519()
// asks WebCrypto first (Chrome and Edge have Ed25519 there from 113); where importKey() refuses the algorithm it
// verifies here instead, in plain JavaScript (BigInt), so that the check is never skipped. The same arithmetic as
// tools/usign-verify.py; tests/web/signature.test.mjs holds both ways to the project's real signature. The only thing
// taken from the platform is SHA-512 (subtle.digest, in every browser with WebCrypto at all); a sha512(bytes) of
// one's own may be passed instead.
const P = (1n << 255n) - 19n;
const Q = (1n << 252n) + 27742317777372353535851937790883648493n;      // the group order
const mod = (a, m = P) => ((a % m) + m) % m;
function pow(b, e, m = P) {
  let r = 1n;
  b = mod(b, m);
  while (e > 0n) { if (e & 1n) r = r * b % m; b = b * b % m; e >>= 1n; }
  return r;
}
const D = mod(-121665n * pow(121666n, P - 2n));
const SQRT_M1 = pow(2n, (P - 1n) / 4n);

// points in extended coordinates [X, Y, Z, T]: x = X/Z, y = Y/Z, x*y = T/Z (RFC 8032 5.1.4)
function add(a, b) {
  const A = mod((a[1] - a[0]) * (b[1] - b[0]));
  const B = mod((a[1] + a[0]) * (b[1] + b[0]));
  const C = mod(2n * a[3] * b[3] * D);
  const Dd = mod(2n * a[2] * b[2]);
  const E = B - A, F = Dd - C, G = Dd + C, H = B + A;
  return [mod(E * F), mod(G * H), mod(F * G), mod(E * H)];
}
function mul(s, pt) {
  let r = [0n, 1n, 1n, 0n];
  while (s > 0n) {
    if (s & 1n) r = add(r, pt);
    pt = add(pt, pt);
    s >>= 1n;
  }
  return r;
}
const equal = (a, b) => mod(a[0] * b[2] - b[0] * a[2]) === 0n && mod(a[1] * b[2] - b[1] * a[2]) === 0n;

function recoverX(y, sign) {
  if (y >= P) return null;
  const x2 = mod((y * y - 1n) * pow(D * y * y + 1n, P - 2n));
  if (x2 === 0n) return sign ? null : 0n;
  let x = pow(x2, (P + 3n) / 8n);
  if (mod(x * x - x2) !== 0n) x = mod(x * SQRT_M1);
  if (mod(x * x - x2) !== 0n) return null;
  if ((x & 1n) !== (sign ? 1n : 0n)) x = P - x;
  return x;
}
const toInt = (bytes) => { let n = 0n; for (let i = bytes.length - 1; i >= 0; i--) n = (n << 8n) | BigInt(bytes[i]); return n; };
function decompress(bytes) {
  if (bytes.length !== 32) return null;
  let y = toInt(bytes);
  const sign = (y >> 255n) === 1n;
  y &= (1n << 255n) - 1n;
  const x = recoverX(y, sign);
  if (x === null) return null;
  return [x, y, 1n, mod(x * y)];
}
const GY = mod(4n * pow(5n, P - 2n));
const G = [recoverX(GY, false), GY, 1n, mod(recoverX(GY, false) * GY)];

const concat = (...parts) => {
  const out = new Uint8Array(parts.reduce((n, p) => n + p.length, 0));
  let o = 0;
  for (const p of parts) { out.set(p, o); o += p.length; }
  return out;
};
const subtleSha512 = async (bytes) => new Uint8Array(await globalThis.crypto.subtle.digest("SHA-512", bytes));

// true when sig (64 bytes) is pub's (32 bytes) signature of msg: in JavaScript, without WebCrypto's Ed25519
export async function ed25519Verify(pub, msg, sig, sha512 = subtleSha512) {
  if (!(pub instanceof Uint8Array) || !(sig instanceof Uint8Array) || pub.length !== 32 || sig.length !== 64) return false;
  const A = decompress(pub), R = decompress(sig.subarray(0, 32));
  if (!A || !R) return false;
  const s = toInt(sig.subarray(32));
  if (s >= Q) return false;
  const h = mod(toInt(await sha512(concat(sig.subarray(0, 32), pub, msg))), Q);
  return equal(mul(s, G), add(R, mul(h, A)));
}

// -> { ok, how: "webcrypto" | "js" }: WebCrypto when it knows Ed25519, else the code above. A key or signature
// WebCrypto refuses (DataError: not a valid point) is tried here too, which says the same thing in the end.
export async function verifyEd25519(pub, msg, sig, cryptoImpl = globalThis.crypto) {
  const subtle = cryptoImpl && cryptoImpl.subtle;
  if (subtle && pub.length === 32 && sig.length === 64) {
    try {
      const key = await subtle.importKey("raw", pub, { name: "Ed25519" }, false, ["verify"]);
      return { ok: await subtle.verify("Ed25519", key, sig, msg) === true, how: "webcrypto" };
    } catch (e) { /* no Ed25519 in this WebCrypto: below */ }
  }
  return { ok: await ed25519Verify(pub, msg, sig), how: "js" };
}
