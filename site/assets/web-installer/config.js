// What the owner of the site sets, in one place.
//
// MIRROR_BASE: where this browser downloads a release's files itself (mirror.js), so that nobody has to download
// them from GitHub and pick them. Empty: there is no mirror, and the page asks for the files. What belongs to it:
//   1. the branch mirror/TAG of this repository, with the files in parts of at most 45 MiB and a manifest
//      (tools/make-release-mirror.sh, pushed by tools/publish-mirror.sh at the end of the Release workflow), which
//      raw.githubusercontent.com serves to a page (GitHub's own release downloads do not answer a page);
//   2. its address here: "https://raw.githubusercontent.com/OWNER/REPO/refs/heads/mirror/" (with the last slash;
//      the tag and the file's name follow);
//   3. https://raw.githubusercontent.com in connect-src of the Content-Security-Policy of the three web-installer
//      pages (tests/test_web.py holds 2 and 3 together).
export const MIRROR_BASE = "https://raw.githubusercontent.com/dikeckaan/mu5358-linux/refs/heads/mirror/";
