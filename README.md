# Windrose Query

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

2. Start your server. The query listener initializes automatically after ~2s (same as the RCON variant).

3. A configuration file is generated at:
   ```
   YourServer/R5/Binaries/Win64/windrosequery/settings.ini
   ```

4. Make sure the UDP port (default `27015`) is open in your firewall.

## Configuration

Edit `windrosequery/settings.ini`:

```ini
[Query]
Port=27015
ServerName=Windrose Server
Map=Windrose
GameFolder=windrose
GameDescription=Windrose
AppId=0
MaxPlayers=64
BotCount=0
VACSecured=false
Private=false
Version=1.0.0
EnableLogging=true
LogFile=windrosequery\query.log
```

Values read from the server's `ServerDescription.json` (`ServerName`, `MaxPlayerCount`, `DeploymentId`, etc.) override the configured defaults when available.

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
