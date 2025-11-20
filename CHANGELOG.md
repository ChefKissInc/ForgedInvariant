# Change log

## v1.4.0 (20/11/2025)

### Enhancements

- Sync TSC early (on ForgedInvariant IOService start).

### Bug Fixes

- On Intel, reset TSC_ADJUST of all CPUs at the same time.

**Full Changelog**: https://github.com/ChefKissInc/ForgedInvariant/compare/v1.3.0...v1.4.0

## v1.3.0 (14/11/2025)

### Enhancements

- Sync to last thread on all macOS versions.
- On Intel, always reset TSC_ADJUST if available.
- Code cleanup.
- Store capabilities using bitfield.
- New boot args: `-FITSCPeriodic` to force periodic sync, `-FIOff`, `-FIBeta`.

### Bug Fixes

- Check for TSC_ADJUST only on Intel.

**Full Changelog**: https://github.com/ChefKissInc/ForgedInvariant/compare/v1.2.0...v1.3.0

## v1.2.0 (15/06/2025)

### Enhancements

- Tahoe support.

**Full Changelog**: https://github.com/ChefKissInc/ForgedInvariant/compare/v1.1.0...v1.2.0

## v1.1.0 (15/03/2025)

### Enhancements

- Lowered minimum macOS version to `10.6`.

**Full Changelog**: https://github.com/ChefKissInc/ForgedInvariant/compare/v1.0.1...v1.1.0

## v1.0.1 (31/12/2024)

### Bug Fixes

- Fixed a code bug causing syncTimer to be null, causing ForgedInvariant's periodic fallback to be dysfunctional.

**Full Changelog**: https://github.com/ChefKissInc/ForgedInvariant/compare/v1.0.0...v1.0.1

## v1.0.0 (27/11/2024)

## 🎉 Initial release!

Supports all 64-bit Intel and AMD CPUs, from El Capitan up to Sequoia.
