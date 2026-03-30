# Radar

Mech Badge firmware for ESP32-S3. BLE proximity scanning, ESP-NOW match handshake, deterministic turn-based combat.

Combat engine ported from [wildcat86/DC34-Mech-Badge](https://github.com/wildcat86/DC34-Mech-Badge). Hardware design at [eggsactly/DC-34-Badge](https://github.com/eggsactly/DC-34-Badge).

## How it works

1. Badge advertises identity over BLE (player ID, faction, mech class, rank)
2. Scanner picks up nearby badges, classifies by RSSI into proximity zones
3. User selects a target from the list, badge sends ESP-NOW match invite with a random session salt
4. Opponent auto-accepts, host sends START
5. Both badges seed a splitmix64 RNG with `make_seed(idA, idB, salt)` — order-independent, both get identical sequences
6. Each turn, players pick a range action + which weapons to fire (5 bits, 1 byte over ESP-NOW)
7. Both badges resolve the turn identically from the shared RNG — initiative, range contest, attacks, damage, heat
8. Match ends when a mech's center torso or head hits 0 internal, or after 30 turns

Only 1 byte per turn needs to go over the air. Everything else is computed locally.

## Proximity zones

- **VERY CLOSE** — >= -60 dBm
- **CLOSE** — >= -70 dBm
- **MEDIUM** — >= -80 dBm
- **FAR** — < -80 dBm

`>>` approaching, `<<` retreating, `--` holding.

## Testing

For testing without a display, the firmware auto-engages the closest badge in CLOSE range. Flash two ESP32-S3 boards, put them near each other, and watch the serial monitors — you'll see the full match play out turn by turn.

```bash
idf.py build
idf.py -p COM4 flash monitor   # board 1
idf.py -p COM5 flash monitor   # board 2
```

## Mechs (from mech_idle.cpp)

**SUMMONER [PRIME]** — speed 3, 6 heat sinks
- PPC (10 dmg, 10 heat, med-long)
- Medium Laser (5 dmg, 3 heat, short-med)
- SRM-4 (8 dmg, 3 heat, melee-short)

**BUSHWACKER [BSW-X1]** — speed 2, 5 heat sinks
- AC/5 (5 dmg, 1 heat, short-long)
- LRM-10 (10 dmg, 4 heat, med-long)
- Medium Laser (5 dmg, 3 heat, short-med)

## Packet flow

```
[Host]                         [Opponent]
  |-- MATCH_INVITE (salt) ------->|
  |<----- MATCH_ACCEPT ----------|
  |-- MATCH_START --------------->|
  |                               |
  |-- TURN_CHOICE (turn 0) ----->|
  |<----- TURN_CHOICE (turn 0) --|
  |   (both resolve turn 0)      |
  |-- TURN_CHOICE (turn 1) ----->|
  |<----- TURN_CHOICE (turn 1) --|
  |   ...                         |
  |-- MATCH_END ----------------->|
```

## Event keys

Replace the zero-filled `BADGE_HMAC_KEY` and `BADGE_PMK` before flashing production badges:

```bash
python3 -c "import os; print(', '.join(f'0x{b:02X}' for b in os.urandom(16)))"
```

## TODO

- 9-DOF heading (reads -1 for now)
- QR code on match end
- display UI (cockpit view, proximity list, combat HUD)
- faction multiplayer
- weapon/mech selection from SAO loadout
