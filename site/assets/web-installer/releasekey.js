// The project's release key, and the check of a release's signature with it. Every release carries SHA256SUMS.sig,
// the usign (OpenBSD signify format, Ed25519) signature of SHA256SUMS by this key (tools/sign-release.sh); the page
// takes a SHA256SUMS only when it has GitHub's digest for the release AND this key's signature, so that a file put
// on GitHub by someone else, even with a matching digest, is refused. RELEASE_PUB is installer/release.pub as it is
// (tests/web/signature.test.mjs holds the two to each other); the number is the key's own, as usign prints it.
import { verifyEd25519 } from "./ed25519.js";
import { parseUsign } from "./flow.js";

export const RELEASE_PUB = `untrusted comment: mu5358-linux release public key 15fd2f702186c19e
RWQV/S9wIYbBnphvmyMWH65u3ymNlDQ6cmQHEqaryboAoqADy0J0vCuR
`;

// a usign public key file -> { number: 16 hex digits, short: its first 8, pub: Uint8Array(32), comment }
export function releaseKey(text = RELEASE_PUB) {
  const k = parseUsign(text, "key");
  if (!k) throw new Error("not a usign public key");
  return { number: k.keyNumber, short: k.keyNumber.slice(0, 8), pub: k.body, comment: k.comment };
}
export const RELEASE_KEY = releaseKey();

// Is sigText (the contents of SHA256SUMS.sig) key's signature of sumsBytes (the whole of SHA256SUMS)?
// -> { ok: true, how: "webcrypto" | "js", keyNumber } or { ok: false, why, keyNumber }, why one of
//    sig-format   not a usign signature file
//    sig-key      a signature by another key (its number is keyNumber), not by key
//    sig-invalid  the key's number, but not its signature of these bytes
export async function verifySums(sumsBytes, sigText, key = RELEASE_KEY, cryptoImpl = globalThis.crypto) {
  const sig = parseUsign(sigText, "signature");
  if (!sig) return { ok: false, why: "sig-format", keyNumber: "" };
  if (sig.keyNumber !== key.number) return { ok: false, why: "sig-key", keyNumber: sig.keyNumber };
  const r = await verifyEd25519(key.pub, sumsBytes, sig.body, cryptoImpl);
  if (!r.ok) return { ok: false, why: "sig-invalid", keyNumber: sig.keyNumber };
  return { ok: true, how: r.how, keyNumber: sig.keyNumber };
}
