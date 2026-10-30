// The project's releases, from GitHub's API (which a page may read: it sends CORS headers). What the page takes from
// an answer: the tag, whether it is a prerelease, and for each file its name, size and the SHA-256 GitHub computed
// when it was uploaded ("digest": "sha256:..."). A release's files themselves cannot be read by a page (GitHub's
// download links send no CORS headers): the user downloads them and picks them here. The page then wants two things
// of SHA256SUMS: GitHub's digest, and the project's signature of it (SHA256SUMS.sig, checked against releasekey.js).
import { newerTag, releaseKey } from "./version.js";

export const REPO = "dikeckaan/mu5358-linux";
// the files of a release, all of which the page needs
export const F = {
  sums: "SHA256SUMS", sig: "SHA256SUMS.sig", image: "mu5358-openwrt.img.gz", kernel: "mu5358-kernel.Image.gz", ramdisk: "mu5358-ramdisk.tar.gz",
  script: "mu5358-install.sh", list: "android-files.txt", tools: "mu5358-tools.tar.gz",
};
// every release has these (SHA256SUMS.sig from v2026.10.30.6 on: a release without it is not offered);
// mu5358-tools.tar.gz (gpt-userdata, f2fs-shrink: the region) from v2026.10.30.2 on
export const REQUIRED_FILES = [F.sums, F.sig, F.image, F.kernel, F.ramdisk, F.script, F.list];
export const RELEASE_FILES = [...REQUIRED_FILES, F.tools];
// the files of this release the page needs: the required ones, and the tools when the release has them
export const filesOf = (rel) => RELEASE_FILES.filter((n) => REQUIRED_FILES.includes(n) || rel.assets.has(n));

export class GithubError extends Error {
  constructor(code, message, extra) { super(message || code); this.name = "GithubError"; this.code = code; Object.assign(this, extra || {}); }
}

const TAG = /^v[0-9][0-9A-Za-z._-]{0,40}$/;

export function cleanRelease(r, repo = REPO) {
  if (!r || typeof r.tag_name !== "string" || !TAG.test(r.tag_name) || r.tag_name.includes("..") || r.draft) return null;
  const assets = new Map();
  for (const a of Array.isArray(r.assets) ? r.assets : []) {
    if (!a || !RELEASE_FILES.includes(a.name)) continue;
    if (!Number.isSafeInteger(a.size) || a.size <= 0) continue;
    const m = /^sha256:([0-9a-f]{64})$/.exec(typeof a.digest === "string" ? a.digest : "");
    assets.set(a.name, {
      name: a.name, size: a.size, sha256: m ? m[1] : null,
      // built here, never taken from the answer: a link on this page goes to this project's release and nowhere else
      url: `https://github.com/${repo}/releases/download/${encodeURIComponent(r.tag_name)}/${encodeURIComponent(a.name)}`,
    });
  }
  const t = Date.parse(r.published_at || "");
  return { tag: r.tag_name, prerelease: !!r.prerelease, published: Number.isFinite(t) ? t : 0, assets,
    page: `https://github.com/${repo}/releases/tag/${encodeURIComponent(r.tag_name)}` };
}

// a release the page can install: every file there, and SHA256SUMS with GitHub's digest (the page's anchor of trust)
export function usable(rel) {
  return REQUIRED_FILES.every((n) => rel.assets.has(n)) && !!rel.assets.get(F.sums).sha256;
}

async function getJson(fetchImpl, url) {
  let res;
  try {
    res = await fetchImpl(url, { headers: { Accept: "application/vnd.github+json" }, credentials: "omit", cache: "no-store", referrerPolicy: "no-referrer" });
  } catch (e) { throw new GithubError("network", "GitHub could not be reached", { cause: e }); }
  if (res.status === 403 || res.status === 429) throw new GithubError("rate-limit", "GitHub's limit for requests without an account is used up for now");
  if (!res.ok) throw new GithubError("http", `GitHub answered ${res.status}`, { status: res.status });
  try { return await res.json(); } catch (e) { throw new GithubError("bad-answer", "GitHub's answer could not be read"); }
}

const newer = (a, b) => (releaseKey(a.tag) && releaseKey(b.tag) && a.tag !== b.tag ? newerTag(a.tag, b.tag) : a.published > b.published);

// loadReleases() -> [release, ...], newest first; only releases that have all the files the page needs
export async function loadReleases(fetchImpl = globalThis.fetch.bind(globalThis), repo = REPO) {
  const list = await getJson(fetchImpl, `https://api.github.com/repos/${repo}/releases?per_page=30`);
  if (!Array.isArray(list)) throw new GithubError("bad-answer", "GitHub's answer could not be read");
  const rel = list.map((r) => cleanRelease(r, repo)).filter((r) => r && usable(r));
  rel.sort((a, b) => (newer(a, b) ? -1 : newer(b, a) ? 1 : 0));
  return rel;
}
