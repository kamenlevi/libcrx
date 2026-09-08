# Corpus

- `oracle-pixls.tsv`: LibRaw fingerprints of the public raw.pixls.us Canon
  CR3 samples (`pixls.tsv` lists model, mode, size, licence, SHA-256, URL).
- `oracle-private.tsv`: fingerprints of the private corpus. No paths.
- `corpus/private/` (ignored): the private manifest lives outside the tree.

Regenerate a fingerprint file with `crxoracle <files> > corpus/oracle-x.tsv`.
A row that starts with `#` is a file the oracle could not fingerprint, with
the reason.
