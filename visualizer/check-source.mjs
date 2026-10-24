// Guards the one claim on the page that nothing else can: that the C++ quoted
// under the book is the C++ that produced the numbers above it.
//
// Two independent things can go wrong, so two things are checked.
//
// The file holds the quotes as plain text and app.js highlights them at runtime, so
// this compares the file's own text against the source and never the highlight.
//
// The quote can drift from src/. Edit order_book.cpp and the page keeps showing
// the old text at the old line numbers, which fails silently, so each excerpt is
// compared against the working tree.
//
// The quote can also drift from the line it links to. A permalink to the wrong
// commit still resolves, still highlights a range, and still shows different
// code from the page, which fails just as silently. So each excerpt is compared
// against the revision in its own link as well.
//
// The second comparison needs that revision in this clone. A shallow checkout
// cannot make it, and that is reported as unverified rather than passing or
// failing, because neither answer would be true.
//
// Not covered: that the excerpts are the most illuminating five, or that the
// prose around them is fair. Those are reading questions and this file has no
// reader.
//
// Run with: node visualizer/check-source.mjs

import { execFileSync } from "node:child_process";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";

const failures = [];
function check(condition, message) {
  if (!condition) failures.push(message);
}

const repoRoot = fileURLToPath(new URL("..", import.meta.url));

function git(args) {
  try {
    return execFileSync("git", args, { cwd: repoRoot, encoding: "utf8", stdio: ["ignore", "pipe", "ignore"] });
  } catch {
    return null;
  }
}

const shallow = git(["rev-parse", "--is-shallow-repository"])?.trim() === "true";
const revisionState = new Map();
function revisionExists(revision) {
  if (!revisionState.has(revision)) {
    revisionState.set(revision, git(["rev-parse", "--verify", "--quiet", `${revision}^{commit}`]) !== null);
  }
  return revisionState.get(revision);
}

const source = (relative) => readFileSync(new URL(`../${relative}`, import.meta.url), "utf8");
const page = source("visualizer/index.html");

// Entities the page has to escape to show C++ as text rather than as markup.
const unescape = (text) => text
  .replace(/&lt;/g, "<")
  .replace(/&gt;/g, ">")
  .replace(/&quot;/g, '"')
  .replace(/&#39;/g, "'")
  .replace(/&amp;/g, "&");

// The walkthrough is a document: a heading, a paragraph, the excerpt, and the
// source link, in one .code-item. The trade-off item has no excerpt and so is not
// one of these, which is why the count asserted below is a number.
const items = [...page.matchAll(/<div class="code-item">([\s\S]*?)<\/div>/g)].map((match) => match[1]);

// A count, not a floor. If the markup is reshaped and the pattern stops matching,
// an empty list would otherwise pass every assertion below it.
check(items.length === 5, `found ${items.length} excerpts in index.html, expected 5`);

function compare(where, label, quoted, lines) {
  if (quoted.length !== lines.length) {
    check(false, `${where}: ${label} has ${lines.length} lines but the page shows ${quoted.length}`);
    return;
  }
  for (const [offset, quotedLine] of quoted.entries()) {
    check(
      quotedLine === lines[offset],
      `${where}: ${label} line ${offset + 1} does not match.\n` +
      `      page:   ${JSON.stringify(quotedLine)}\n` +
      `      source: ${JSON.stringify(lines[offset])}`,
    );
  }
}

const seen = new Set();
const verifiedRevisions = new Set();
const unverifiedRevisions = new Set();

for (const [index, item] of items.entries()) {
  const where = `excerpt ${index + 1}`;
  const link = item.match(/<a class="code-ref" href="([^"]+)">([^<]+)<\/a>/);
  const block = item.match(/<pre class="code-block"><code>([\s\S]*?)<\/code><\/pre>/);
  check(Boolean(link), `${where}: no code-ref link`);
  check(Boolean(block), `${where}: no code block`);
  if (!link || !block) continue;

  const [, href, label] = link;
  const target = href.match(/^https:\/\/github\.com\/wheevu\/orda-book\/blob\/([0-9a-f]{7,40})\/(.+)#L(\d+)-L(\d+)$/);
  check(
    Boolean(target),
    `${where}: ${href} is not a permalink to a line range. A link to a branch drifts ` +
    "away from the quoted lines the moment the file above them changes.",
  );
  if (!target) continue;

  const [, revision, path, startText, endText] = target;
  const start = Number(startText);
  const end = Number(endText);
  check(end >= start, `${where}: ${path} range runs backwards (${start}-${end})`);

  // The visible caption and the link have to say the same thing, or the reader
  // is told one line number and sent to another.
  check(
    label === `${path.split("/").pop()}:${start}`,
    `${where}: the caption says "${label}" but the link points at ${path}:${start}`,
  );

  const quoted = unescape(block[1]).split("\n");
  const key = `${path}:${start}-${end}`;
  check(!seen.has(key), `${where}: ${key} is quoted twice`);
  seen.add(key);

  compare(where, `${key} in the working tree`, quoted, source(path).split("\n").slice(start - 1, end));

  if (!revisionExists(revision)) {
    // A shallow checkout genuinely cannot answer this. A full clone can, so an
    // unknown revision there is a broken link rather than a limit of the clone.
    if (shallow) {
      unverifiedRevisions.add(revision);
    } else {
      check(false, `${where}: ${revision} is not a commit in this repository, so ${href} is a dead link`);
    }
    continue;
  }

  const blob = git(["show", `${revision}:${path}`]);
  if (blob === null) {
    check(false, `${where}: ${path} does not exist at ${revision}`);
    continue;
  }
  verifiedRevisions.add(revision);
  compare(where, `${key} at ${revision}`, quoted, blob.split("\n").slice(start - 1, end));
}

if (failures.length > 0) {
  console.error(`${failures.length} source excerpt check(s) failed:`);
  for (const failure of failures) console.error(`  - ${failure}`);
  process.exit(1);
}

console.log(
  `source ok: ${items.length} excerpts match src/ verbatim at their quoted line numbers` +
  (verifiedRevisions.size > 0
    ? `, and again at ${[...verifiedRevisions].join(", ")}`
    : ""),
);
if (unverifiedRevisions.size > 0) {
  console.warn(
    `note: shallow checkout, so ${[...unverifiedRevisions].join(", ")} could not be resolved ` +
    "and the permalinks were not checked against them",
  );
}
