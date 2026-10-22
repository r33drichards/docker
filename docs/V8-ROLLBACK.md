# V8 extension rollback

This is a single aggregate inverse of mainline merges #3, #4, #6, #7,
#13, #14, #16, #18 and #19, from master `19120a1` back toward the
pre-extension baseline `92cd2c5`. Shared history is not rewritten.

## Scope

Removed: JavaScript/V8 and Wasm scripting, command observation debug module,
script hooks/mutations/host APIs, native fetch and webhooks, NocoDB forwarding,
script SQLite APIs, RSS subscriptions/commands, and their examples/docs/CI.

Retained beyond the baseline: InspIRCd v4.12.1, pinned CI actions and safe
publication boundaries, the unattended-test exclusion for the interactive bot
helper, persistent configuration/data/log layout, checksum-managed seeding,
guarded TLS import, and bounded logging. Existing metrics, configurable
maxchans, bot-oper and flood settings remain unchanged. This is a feature
rollback, not a byte-for-byte historical tree restoration.

The legacy `/sqlite-bootstrap` executable name and `script-databases` directory
initialization remain to preserve the deployed startup/volume contract. Neither
requires SQLite or V8, opens database files, nor removes existing data.
The original seed baseline hashes remain migration metadata, not active
configuration includes. The obsolete NocoDB environment token is cleared before
running any child process; no new token file is materialized.

## Deployment prerequisite: audit persisted configuration

Do not deploy until this check is complete. The rollback does not change live
configuration or deploy anything. A code revert alone cannot guarantee startup
against an arbitrary user-edited mounted configuration.

1. Back up the persistent configuration and verify a recovery path. Keep the
   existing volume, ownership, TLS files, database files, and script files.
2. Inspect the actual root configuration, all recursive file/directory includes,
   and generated executable includes. Remove or disable loads of `javascript`
   / `m_javascript`, the removed `precommand` debug module, JavaScript/Wasm/RSS
   configuration, and executable includes of `nocodb.sh` or `sqlite.sh`.
   Preserve unrelated custom settings and bounded logging/persistence includes.
3. Check environment-selected startup arguments and external scripts for the
   removed commands/modules. Stop dependent RSS/NocoDB jobs explicitly.
4. Validate that exact configuration with the rollback image before replacing
   the service, and verify IRC/relay connectivity afterward.

Managed, unchanged defaults are updated by the existing seeder. User-edited
files are intentionally never overwritten. Files removed from the image remain
on the persistent disk as unmanaged files; this includes old helper scripts.
Do not assume an old include is safe merely because its target still exists.
Do not delete volumes, scripts or SQLite databases to complete this rollback.
