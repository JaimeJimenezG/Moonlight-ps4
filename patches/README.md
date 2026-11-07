# Patches applied on top of pinned third_party checkouts
#
# After `scripts/linux/setup_deps.sh` (or a clean submodule checkout), run
# `scripts/linux/apply_patches.sh`. Working trees under third_party/ will look
# dirty — that is expected; do not commit those changes into the submodules.
#
# | Patch | Target | Purpose |
# |-------|--------|---------|
# | `moonlight-common-c-orbis.patch` | `third_party/moonlight-common-c` @ e41355e | Orbis sockaddr family (`LC_ADDR_FAMILY`), IPv4-only resolve, UDP protocol 0 workaround, ENet bind avoidance |
# | `moonlight-common-c-enet-orbis.patch` | `…/enet` @ aca8784 | Same BSD/`ss_family` layout fix inside ENet (`ENET_SS_FAMILY`) |
#
# Regenerate after intentional edits:
#   scripts/linux/refresh_patches.sh
