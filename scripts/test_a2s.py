"""
Simple A2S (Source Server Query) test client for Windrose Query.

Usage:
    python test_a2s.py [host] [port]

Sends A2S_INFO, A2S_PLAYER and A2S_RULES queries, handling the S2C_CHALLENGE
response required by modern Valve servers.
"""

import socket
import struct
import sys
import time


HEADER_SIMPLE = b"\xFF\xFF\xFF\xFF"

REQ_INFO   = 0x54
REQ_PLAYER = 0x55
REQ_RULES  = 0x56

RESP_INFO      = 0x49
RESP_PLAYER    = 0x44
RESP_RULES     = 0x45
RESP_CHALLENGE = 0x41


def read_cstring(buf: bytes, offset: int) -> tuple[str, int]:
    end = buf.index(b"\x00", offset)
    return buf[offset:end].decode("utf-8", errors="replace"), end + 1


def send_recv(sock: socket.socket, addr: tuple[str, int], payload: bytes) -> bytes:
    sock.sendto(HEADER_SIMPLE + payload, addr)
    data, _ = sock.recvfrom(4096)
    return data


def query_info(sock, addr) -> dict:
    payload = struct.pack("<B", REQ_INFO) + b"Source Engine Query\x00"
    data = send_recv(sock, addr, payload)
    assert data.startswith(HEADER_SIMPLE)
    kind = data[4]
    if kind == RESP_CHALLENGE:
        challenge = data[5:9]
        data = send_recv(sock, addr, payload + challenge)
        kind = data[4]
    assert kind == RESP_INFO, f"Unexpected info response: 0x{kind:02x}"

    off = 5
    protocol = data[off]; off += 1
    name, off = read_cstring(data, off)
    map_, off = read_cstring(data, off)
    folder, off = read_cstring(data, off)
    game, off = read_cstring(data, off)
    app_id = struct.unpack_from("<H", data, off)[0]; off += 2
    players = data[off]; off += 1
    max_players = data[off]; off += 1
    bots = data[off]; off += 1
    server_type = chr(data[off]); off += 1
    environment = chr(data[off]); off += 1
    visibility = data[off]; off += 1
    vac = data[off]; off += 1
    version, off = read_cstring(data, off)

    return {
        "protocol": protocol,
        "name": name,
        "map": map_,
        "folder": folder,
        "game": game,
        "app_id": app_id,
        "players": players,
        "max_players": max_players,
        "bots": bots,
        "server_type": server_type,
        "environment": environment,
        "visibility": visibility,
        "vac": vac,
        "version": version,
    }


def challenged_request(sock, addr, kind: int) -> bytes:
    # Request with -1 to ask for a challenge token first.
    payload = struct.pack("<Bi", kind, -1)
    data = send_recv(sock, addr, payload)
    assert data.startswith(HEADER_SIMPLE)
    if data[4] == RESP_CHALLENGE:
        token = data[5:9]
        payload = struct.pack("<B", kind) + token
        data = send_recv(sock, addr, payload)
    return data


def query_players(sock, addr) -> list[dict]:
    data = challenged_request(sock, addr, REQ_PLAYER)
    assert data[4] == RESP_PLAYER
    count = data[5]
    off = 6
    out = []
    for _ in range(count):
        index = data[off]; off += 1
        name, off = read_cstring(data, off)
        score = struct.unpack_from("<i", data, off)[0]; off += 4
        duration = struct.unpack_from("<f", data, off)[0]; off += 4
        out.append({"index": index, "name": name, "score": score, "duration": duration})
    return out


def query_rules(sock, addr) -> dict:
    data = challenged_request(sock, addr, REQ_RULES)
    assert data[4] == RESP_RULES
    count = struct.unpack_from("<H", data, 5)[0]
    off = 7
    rules = {}
    for _ in range(count):
        k, off = read_cstring(data, off)
        v, off = read_cstring(data, off)
        rules[k] = v
    return rules


def main() -> int:
    host = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 27015
    addr = (host, port)

    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.settimeout(3.0)

        t0 = time.time()
        info = query_info(sock, addr)
        print(f"A2S_INFO ({(time.time() - t0) * 1000:.1f} ms)")
        for k, v in info.items():
            print(f"  {k}: {v}")

        players = query_players(sock, addr)
        print(f"A2S_PLAYER ({len(players)} players)")
        for p in players:
            print(f"  [{p['index']}] {p['name']}  score={p['score']}  duration={p['duration']:.0f}s")

        rules = query_rules(sock, addr)
        print(f"A2S_RULES ({len(rules)} rules)")
        for k, v in rules.items():
            print(f"  {k} = {v}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
