# Cumulative adaptive language-model history

This branch starts at upstream `9ed13b335b560b838b77ed9eab5330598dbc021e`.
The adaptive history store uses permanent counts instead of recent pools.
Pinyin segmentation and dictionary costs retain the upstream formulas.
`UserLanguageModel` directly mixes base and history probabilities with a default
history weight of 99%, without a base-probability floor. A separate per-user
deletion filter now excludes explicitly deleted
pinyin/text identities during matching and candidate enumeration. There is no
lexicographic learning rank or character-suffix history model.

## Counts and probabilities

Each learned word/code identity adds one to its unigram count `F(w)`. Adjacent
words add one to `F(p,w)`. The upstream `<s>` / `</s>` boundary observations and
`addWithContext` sentence-extension behavior are retained: an extension adds only
the new words and edges, replacing the latest provisional end-of-sentence edge.
It does not learn the previous context a second time.

All observations have weight one, stored as unsigned 64-bit integers. There are
no history pools, age weights, capacity eviction, or stored percentages. A word
with count 100 keeps count 100 when a different word is selected.

For scoring, `N` is the sum of all stored unigram counts, including the upstream
boundary observations. The history probability is:

```
H(w|p) = 0.2 * F(w) / (N + 0.5)
       + 0.8 * F(p,w) / (F(p) + 0.5)
```

The upstream unigram-only option uses `F(w)/(N+0.5)` instead. The upstream
0.5-count denominator smoothing, upper clamp at 1, unknown penalty, and log10
conversion remain. `N` replaces the old pool-capacity denominator in absolute
count units; pool-derived normalization and age weights no longer exist.
Normalization can change a probability without changing any unrelated count.

The base/history combination uses default weights of 1% base and 99% history,
without the upstream lower bound at the base probability:

```
P(w|p) = 0.01 * P_base(w|p) + 0.99 * H(w|p)
```

The existing configurable history weight and unigram-only options remain.
Scoring uses indexed lookups and cached totals, including totals for queries
with omitted validation codes. It never scans all historical observations.
Totals are updated with learning and forgetting, and rebuilt while loading.

## Forget and clear

`forget(w)` halves each stored unigram identity for that word and each incoming
bigram record `(p,w)`, using integer division. A code-specific forget affects
only records with that exact target code. Records reaching zero are removed.
Other words' unigrams and outgoing edges to other words remain untouched.
A self-edge `(w,w)` is an incoming edge and is therefore halved once.

Forgetting does not delete a user dictionary entry or an explicit pin. Those
objects belong to their existing dictionary/plugin owners, not this history
store. `clear()` still discards the entire history and latest append context;
the caller's existing complete-user-data clearing flow is unchanged.

## Persistence

Binary version 5 stores exact unigram and bigram counts plus the latest append
context, with Zstandard compression. It does not persist sentence replay pools
or redundant query aggregates. Versions 1–4 are readable: each retained legacy
sentence is imported once, independent of which old pool held it. Samples that
an old version already evicted cannot be recovered from that file.

The text format begins with `# libime-history-counts-v1`. `U` records contain a
count, word and code; `B` records contain a count and both word/code identities;
`L` records contain the latest append context. Strings use the existing Fcitx
value escaping, with explicit quoted empty fields. This format preserves
independently halved counts, which a list of whole sentences cannot represent.
The history tool can round-trip both formats. Legacy sentence-list text remains
readable. Older libime versions cannot read a newly saved version-5 file.

No database migration, installation, or live-user-data conversion is performed
by changing branches or building this code.

## Separate halve-history and delete-word actions

`fcitx5-chinese-addons` supplies two configurable actions, each opening the
upstream candidate-selection interface. Choose a candidate with its number key;
Escape cancels. The corresponding candidate context-menu actions are separate.

* `HalveHistory`, default `Control+Alt+7`: calls libime's existing `forget()`.
  The prior cumulative-history policy remains: each distinct constituent word
  in a compound candidate is halved once. User entries and explicit pins remain.
* `ForgetWord`, default `Control+7`: keeps the original shortcut/configuration
  key but now deletes the selected complete pinyin/text identity. The action
  removes the exact user-dictionary entry and its exact-code unigram and both
  incoming and outgoing history records. Unlike the author's sentence removal,
  unrelated words' unigram counts are not cleared. Compound candidates delete
  the whole selected identity, not their individual characters or constituent
  words.

Read-only system and built-in dictionaries receive a per-user exclusion overlay;
shared dictionary files are not rewritten. libime checks the overlay on ordinary
pinyin dictionary matches and rejects automatic multiword candidates that
reconstruct the excluded identity. Prefix and cursor-limited candidates are
checked too. The pinyin parser and scoring formulas are unchanged. A revision
invalidates cached lattice matches after an exclusion or restoration; indexed
identity lookups do not scan user history or all excluded words when scoring.

`fcitx5-chinese-addons` persists exclusions as full pinyin and text in
`pinyin/deleted-words.conf` under the user's Fcitx data directory. Learning history
and exclusions are separate. `clearalldict` clears the user dictionary, history,
and exclusions; `clearuserdict` continues to clear only the user dictionary.

libime restores an excluded identity only when an actual normal pinyin commit
contains that complete pinyin/text identity assembled from the user's selected
pieces. Merely loading a dictionary, waiting, or typing the pinyin does not
restore the identity. Selecting the same text with a different reading does not
restore the deleted reading. `fcitx5-chinese-addons` saves restored exclusions
when the commit occurs. Existing custom-phrase, cloud, and model-only prediction
candidate interfaces retain their own upstream behavior; the deletion/halving
selection interface operates on ordinary pinyin candidates, as upstream does.

`contrib/fcitx5-chinese-addons-forget.patch` contains the complete companion
change against Chinese-addons upstream `bc84e3a`. The Chinese-addons working
branch `cumulative-history` contains the same change. Editing these sources
does not update the installed binaries or live user data.
