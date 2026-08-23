# Releasing

How to cut a release, and the one distinction that is easy to get wrong.

---

## Tag push or manual dispatch?

`.github/workflows/release.yml` has two triggers. They run the same build and
produce the same three artifacts, but they are **not** interchangeable.

| | Tag push (`v*`) | Manual dispatch |
|---|---|---|
| What gets built | the commit you tagged | whatever branch you picked in **Use workflow from** |
| The `tag` input | n/a | a **label only** — it does not select a commit |
| Marked prerelease | no | **yes** |
| Seen by the in-app update check | yes, once published | **never** |
| Created as a draft | yes | yes |

Two things follow from that table.

**The dispatch `tag` input does not check anything out.** `actions/checkout` in
this workflow pins no `ref`, so a dispatch builds the ref chosen in the dropdown
and merely *names* the result after the string you typed. Dispatching `v2.0.1`
from `master` builds `master` HEAD and labels it v2.0.1, whether or not a
`v2.0.1` tag exists or points somewhere else entirely.

**A dispatched release is invisible to users.** The publish step sets
`prerelease: ${{ github.event_name == 'workflow_dispatch' }}`, and the update
checker asks GitHub for `/releases/latest`
(`qimgv/components/updatechecker.cpp`), an endpoint that returns the latest
release which is neither a draft nor a prerelease. So a dispatched build stays
hidden from *Check for updates* even after you publish it.

**Use a tag push for anything real. Use dispatch to test the pipeline off a
branch.**

---

## Cutting a release

### 1. Bump the version

`project(VERSION ...)` in `CMakeLists.txt` is authoritative — the workflow reads
it and refuses to build if it disagrees with the tag. Bump these together:

- `CMakeLists.txt` — `project(qimgv VERSION x.y.z ...)`
- `qimgv/distrib/qimgv.appdata.xml` — a new `<release version="x.y.z" date="...">`
  entry at the top of `<releases>`

### 2. Write the changelog section

Add a section to `CHANGELOG.md`. **The heading must be exactly `## x.y.z`** —
bare, all three segments, no `v`, no date, no codename:

```markdown
## 2.0.1
```

This is a lookup key, not a style choice. `Core::changelogForCurrentVersion()`
searches the file for `"## " + appVersion.toString()`, and the section it finds
is what the "what's new" window shows after an update. A miss is **silent**: the
window simply never appears and the user gets a one-line toast instead. Note
`QVersionNumber(2,1,0).toString()` is `"2.1.0"`, so `## 2.1` would never match.

Write it for a user reading it in that window, not as a list of commit subjects.
Leave out anything that was broken and fixed within the same release — it never
shipped, so it is not news.

### 3. Land it, then tag

```bash
git push origin master
git tag -a v2.0.1 -m "qimgv 2.0.1"
git push origin v2.0.1
```

Push the branch **first**. Tagging a commit the remote does not have yet means
the workflow builds a commit that is not on `master`.

### 4. Publish

The workflow produces three artifacts plus `SHA256SUMS.txt`, refuses to publish
if any is missing, and creates the release as a **draft**:

- `qimgv-x.y.z-win64-setup.exe` — the per-user installer
- `qimgv-x.y.z-win64.zip` — portable
- `qimgv-x.y.z-win64-minimal.zip` — no video playback

Review the draft on GitHub and press **Publish release**. Until you do, the
update check keeps reporting "up to date" — a draft is not `/releases/latest`.

---

## If the build fails on the version gate

```
::error::tag v2.0.1 does not match project(VERSION) 2.0.0 in CMakeLists.txt.
```

You tagged without bumping. Fix `CMakeLists.txt` and `qimgv.appdata.xml`, then
move the tag:

```bash
git tag -d v2.0.1
git push origin :refs/tags/v2.0.1
# commit the bump, then re-tag
```

An `-rc1` or `-beta` suffix on the tag is allowed and compares as its base
version, so `v2.1.0-rc1` matches `project(VERSION 2.1.0)`.

---

## Checklist

- [ ] `CMakeLists.txt` version bumped
- [ ] `qimgv/distrib/qimgv.appdata.xml` release entry added
- [ ] `CHANGELOG.md` section added, heading exactly `## x.y.z`
- [ ] CI green on the commit being tagged
- [ ] branch pushed, then tagged, then the tag pushed
- [ ] draft reviewed and published
