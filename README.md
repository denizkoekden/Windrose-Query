# Windrose Query

[![build](https://github.com/denizkoekden/windrose-query/actions/workflows/build.yml/badge.svg)](https://github.com/denizkoekden/windrose-query/actions/workflows/build.yml)

Standalone A2S (Source Server Query) provider for Windrose game servers. Injects via `version.dll` proxy and exposes the Valve Source query protocol on UDP so tools such as the Steam server browser, Gametracker, BattleMetrics, `a2s`, `python-valve`, `rust-a2s`, etc. can read server info, players and rules.

## Features

- **A2S_INFO** - Server name, map, players, version, VAC state
- **A2S_PLAYER** - Connected players with name, score and connection duration
- **A2S_RULES** - Exposes server metadata (invite code, deployment id, address)
- **S2C_CHALLENGE** - Full support for the challenge token flow required by modern Valve queries
- **Configuration** - Settings stored in `windrosequery/settings.ini`
- **Logging** - All activity logged to `windrosequery/query.log`

## Installation

1. Copy `version.dll` to your server's binaries folder:
   ```
   YourServer/R5/Binaries/Win64/version.dll
   ```

2. Start your server with the usual `-QueryPort` (and optionally `-MultiHome`) switch. Example:
   ```
   WindroseServer.exe -QueryPort=27015 -MultiHome=123.45.67.89
   ```

3. The query listener starts automatically ~2 seconds into startup and binds to the port you passed on the CLI.

4. Open that UDP port in your firewall.

## Configuration

There is no config file. The only knobs are the Unreal command-line switches the server is already started with:

| Switch | Purpose | Default when absent |
|---|---|---|
| `-QueryPort=<port>` | UDP port the A2S listener binds to | `27015` |
| `-MultiHome=<ip>`   | Local IPv4 to bind on | `0.0.0.0` (all interfaces) |

Both `-Key=Value` and the Unreal URL-form `?Key=Value` (e.g. `MapName?listen?QueryPort=27016`) are accepted.

All server metadata reported over A2S (`name`, `max_players`, `version`, `invite_code`, `deployment_id`) is read live from the server's `ServerDescription.json`. Built-in fallbacks in `config.h` are used only if that file is missing.

A log is always written to `YourServer/R5/Binaries/Win64/windrosequery/query.log`.

## Testing

Use any A2S-compatible client. A Python test client is included:

```bash
python scripts/test_a2s.py 127.0.0.1 27015
```

Expected output:

```
A2S_INFO (1.4 ms)
  protocol: 17
  name: My Windrose Server
  map: Windrose
  ...
A2S_PLAYER (2 players)
  [0] Alice  score=0  duration=120s
  [1] Bob    score=0  duration=35s
A2S_RULES (7 rules)
  game = Windrose
  map = Windrose
  invite_code = XXXX
  ...
```

## Building

### Requirements
- xmake v3.0.2+
- Visual Studio 2022
- Windows SDK

### Build Steps
```bash
xmake
```

Output: `dist/version.dll`

### Prebuilt binaries

Every push to this repository runs the `build` workflow and attaches the resulting `version.dll` as a GitHub Actions artifact. Pushing a tag matching `v*` (e.g. `v1.0.0`) additionally creates a GitHub Release with the DLL attached.

## Technical Details

### How It Works

1. **DLL Proxy Injection** - `version.dll` proxies the system `version.dll`, forwarding all `VerQueryValue*`/`GetFileVersionInfo*` exports while attaching to the host process.
2. **GObjects Scanning** - Locates Unreal Engine's global object array using the offset from the Dumper7 SDK and walks it on each query.
3. **Player Validation** - Validates player states via known SDK offsets:
   - `0x0340` - PlayerName (FString)
   - `0x0388` - AccountId (FString)
   - `0x02B2` - PlayerFlags (filters inactive/spectator)
   - `0x02B8` - Score (float)
   - `0x02C8` - StartTime (int32)
4. **A2S Protocol** - Implements the Valve Source server query protocol:
   - Request kinds: `0x54` INFO, `0x55` PLAYER, `0x56` RULES, `0x69` PING
   - Response kinds: `0x49` INFO, `0x44` PLAYER, `0x45` RULES, `0x41` CHALLENGE
   - Challenge tokens are issued per source address and validated on PLAYER/RULES/INFO.

### SDK References

All offsets are documented with references to the Dumper7 SDK:
- `SDK/Basic.hpp` - GObjects offsets
- `SDK/Engine_classes.hpp` - `APlayerState` layout
- `SDK/R5DataKeepers_classes.hpp` - Account data structure

### Credits

- [WindroseRCON](https://github.com/dkoz/WindroseRCON) - DLL proxy pattern and engine integration this project is modeled after
- [Dumper7](https://github.com/Encryqed/Dumper-7) - SDK generator
- [Valve Developer Wiki - Server queries](https://developer.valvesoftware.com/wiki/Server_queries) - Protocol reference
