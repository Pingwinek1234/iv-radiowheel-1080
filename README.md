# Radio Wheel IV – iv-sdk port (GTA IV / EFLC 1.0.8.0)

Port of gennariarmando_AG's **Radio Wheel IV** (plugin-sdk, Complete Edition) to
zolika1351's **iv-sdk** (1.0.8.0). Hold the "previous radio" button in a vehicle to open the wheel,
point at a station with the mouse or right stick, release to confirm. A short tap on next/previous
switches station as before.

**Status: source only, not compiled or run in-game.** It was written without access to the game or
MSVC. A syntax check against the iv-sdk headers passed for `dllmain.cpp`, nothing more.

## Build
1. Copy this folder to `iv-sdk-main/examples/RadioWheelIV/` (so `../../../include/IVSDK.cpp` resolves).
2. Open `build/IVSDKPlugin.sln` in Visual Studio (v142 toolset, DirectX SDK installed, like the SDK examples).
3. Build Release|Win32 → `bin/RadioWheelIV.asi`.

## Install
- `RadioWheelIV.asi` and `RadioWheelIV.ini` → `<GTA IV>/plugins/`
- `resources/pc/textures/*.wtd` → `<GTA IV>/pc/textures/` (the textures are loaded from `platform:/textures/radiowheel[_tlad|_tbogt]`)
- Map "previous radio" to a key you can hold (see original README: default "Wheel Dn" only allows quick cycling).

## What changed vs. the original
The original hooks the game's radio code by pattern scanning (Complete Edition) and draws through
internal HUD/render functions. iv-sdk 1.0.8.0 exposes none of those, so the port is built on what it does have:

| Original | Port |
|---|---|
| Replaces `audRadioAudioEntity::UpdatePlayerVehicleRadio` | Reads the player pad in `processPadEvent`; retunes with `RETUNE_RADIO_UP/DOWN/TO_STATION_INDEX` natives |
| Station count/name from `audRadioStation` | `CAERadioTrackManager::m_aRadioStations` + `TheText.Get(name)` |
| `CSprite2d::Draw`, `CFont::PrintString` via draw callbacks | `DRAW_SPRITE`, `DRAW_RECT`, `DISPLAY_TEXT_WITH_LITERAL_STRING` natives each frame |
| `SpriteLoader` | `CTxdStore::LoadTxd` + `CSprite2d::SetTexture` (same pattern as the SDK's TxdExample) |
| Camera lock via `m_bDisableControls` | Look/mouse pad inputs zeroed while the wheel is open |
| Slow motion | `CTimer::ms_fTimeScale` (same as the original) |

## Known differences / limitations
- **Default radio HUD is not hidden.** The original NOPs the call that draws it; that address is not
  available for 1.0.8.0. The vanilla HUD may show alongside the wheel.
- **No blurred background.** The original's blur shader needs internal post-FX access. `bScreenOverlay` now dims and tints instead.
- **No track names** under the station name (the original read the current track id from the audio entity and a hash→name table).
- "Radio Off" label is English only; station names use the game's text.
- Mouse camera lock is best effort.
- Station icons are looked up as `radio_<station index>`, assuming the wtd textures are numbered by the game's station index.

## Things to check first in-game
These rest on assumptions I could not verify:
1. `DRAW_SPRITE` accepting the `grcTexture*` from `CSprite2d` as its texture handle.
2. `GET_PLAYER_RADIO_STATION_INDEX` returning an out-of-range value when the radio is off, and "off" sitting between the last station and station 0 in the `RETUNE_RADIO_UP/DOWN` cycle.
3. Zeroing `INPUT_VEH_PREV/NEXT_RADIO` in the pad hook actually stopping the game's own radio switching (set `bBlockGameRadioInput FALSE` if it breaks the buttons).
4. `DRAW_RECT` / `DRAW_SPRITE` using centre + size in 0-1 screen fractions.
