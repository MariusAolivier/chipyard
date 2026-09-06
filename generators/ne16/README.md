# Chipyard NE16 integration

This directory integrates the upstream PULP NE16 RTL without modifying it.

## Pinned inputs

- Chipyard: `e602d917dcc495c58cabe906535e411707096c9c`
- NE16: `6bd727206d49e5febbe4d1037eda91d064d203e1`
- Bender: `v0.32.1`
- Bender Linux x86-64 archive SHA-256:
  `59a36723b056a06b266dc68d4ceedcd0aa17a1c096e8c2ea512af264ff6a13f6`

`Bender.lock` records immutable revisions for NE16's transitive RTL
dependencies. Run Bender from this directory so those exact revisions are
used.
