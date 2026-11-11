## What and why

## Checklist

- [ ] Host tests pass (`ctest --test-dir build --output-on-failure`)
- [ ] Mac targets build if Mac code changed (`scripts/build-mac.sh MoofPortInstall`, ...)
- [ ] Tests added or updated for new behavior
- [ ] Resident memory impact stated (driver size change, if any); `test_budget` still passes
- [ ] No Apple software, ROMs, disk images or built binaries are included
