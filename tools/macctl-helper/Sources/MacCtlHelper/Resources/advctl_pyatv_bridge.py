#!/usr/bin/env python3
import argparse
import asyncio
import json
import sys


def emit(value):
    print(json.dumps(value, ensure_ascii=False), flush=True)


def protocol_from_name(name):
    from pyatv.const import Protocol

    normalized = name.replace("-", "").replace("_", "").lower()
    mapping = {
        "airplay": Protocol.AirPlay,
        "mrp": Protocol.MRP,
        "companion": Protocol.Companion,
        "raop": Protocol.RAOP,
        "dmap": Protocol.DMAP,
    }
    return mapping[normalized]


def enum_name(value):
    return getattr(value, "name", str(value).split(".")[-1])


async def make_storage(path):
    from pyatv.storage.file_storage import FileStorage

    loop = asyncio.get_event_loop()
    storage = FileStorage(path, loop)
    await storage.load()
    return storage


async def scan_configs(storage, timeout=5, identifier=None):
    import pyatv

    loop = asyncio.get_event_loop()
    kwargs = {"timeout": timeout, "storage": storage}
    if identifier:
        kwargs["identifier"] = identifier
    return await pyatv.scan(loop, **kwargs)


def device_identifier(config):
    identifiers = list(getattr(config, "identifiers", []) or [])
    return identifiers[0] if identifiers else str(getattr(config, "address", ""))


def device_to_json(config):
    device_info = getattr(config, "device_info", None)
    model = ""
    if device_info is not None:
        model = str(getattr(device_info, "model", "") or getattr(device_info, "model_str", "") or "")

    services = []
    for service in getattr(config, "services", []):
        services.append({
            "proto": enum_name(getattr(service, "protocol", "")).lower(),
            "port": getattr(service, "port", None),
            "pairing": enum_name(getattr(service, "pairing", "")),
            "enabled": bool(getattr(service, "enabled", True)),
            "hasCredentials": getattr(service, "credentials", None) is not None,
        })

    return {
        "id": device_identifier(config),
        "name": getattr(config, "name", "") or "Apple TV",
        "address": str(getattr(config, "address", "")),
        "model": model,
        "services": services,
    }


async def find_config(storage, identifier, timeout=5):
    configs = await scan_configs(storage, timeout=timeout, identifier=identifier)
    if not configs:
        configs = await scan_configs(storage, timeout=timeout)
    for config in configs:
        if identifier in list(getattr(config, "identifiers", []) or []) or identifier == str(getattr(config, "address", "")):
            return config
    raise RuntimeError(f"Apple TV not found: {identifier}")


async def command_scan(args):
    storage = await make_storage(args.storage)
    configs = await scan_configs(storage, timeout=args.timeout)
    await storage.save()
    emit({"devices": [device_to_json(config) for config in configs]})


async def command_pair(args):
    import pyatv

    storage = await make_storage(args.storage)
    config = await find_config(storage, args.identifier)
    proto = protocol_from_name(args.protocol)
    loop = asyncio.get_event_loop()
    pairing = await pyatv.pair(config, proto, loop, storage=storage)
    try:
        await pairing.begin()
        device_provides_pin = bool(pairing.device_provides_pin)
        generated_pin = None
        if not device_provides_pin:
            generated_pin = "1111"
            pairing.pin(int(generated_pin))
        emit({
            "event": "pin_required",
            "message": "Enter the PIN shown by Apple TV" if device_provides_pin else "Enter this PIN on Apple TV",
            "deviceProvidesPin": device_provides_pin,
            "pin": generated_pin,
        })
        raw_pin = sys.stdin.readline().strip()
        if device_provides_pin:
            pairing.pin(int(raw_pin))
        await pairing.finish()
        await storage.save()
        emit({"event": "paired", "paired": bool(pairing.has_paired)})
    finally:
        await pairing.close()


async def connect_atv(args):
    import pyatv

    storage = await make_storage(args.storage)
    config = await find_config(storage, args.identifier)
    loop = asyncio.get_event_loop()
    return await pyatv.connect(config, loop, storage=storage)


async def command_volume(args):
    atv = await connect_atv(args)
    try:
        delta = int(args.delta)
        for _ in range(min(abs(delta), 20)):
            if delta > 0:
                await atv.audio.volume_up()
            elif delta < 0:
                await atv.audio.volume_down()
        volume = getattr(atv.audio, "volume", None)
        emit({"ok": True, "volume": volume})
    finally:
        await asyncio.gather(*atv.close(), return_exceptions=True)


async def command_remote(args):
    atv = await connect_atv(args)
    try:
        remote = atv.remote_control
        command = getattr(remote, args.command)
        await command()
        emit({"ok": True})
    finally:
        await asyncio.gather(*atv.close(), return_exceptions=True)


async def command_state(args):
    atv = await connect_atv(args)
    try:
        try:
            playing = await atv.metadata.playing()
        except Exception:
            playing = None

        title = getattr(playing, "title", None) or ""
        artist = getattr(playing, "artist", None) or getattr(playing, "album", None) or ""
        position = float(getattr(playing, "position", 0) or 0)
        total_time = float(getattr(playing, "total_time", 0) or 0)
        state_text = enum_name(getattr(playing, "device_state", "")) if playing else ""
        progress = int(round(position / total_time * 100)) if total_time > 0 else 0
        volume = getattr(atv.audio, "volume", None)
        volume_percent = int(round(float(volume))) if volume is not None else 0
        emit({
            "active": bool(title or artist or state_text.lower() in ("playing", "paused")),
            "playing": state_text.lower() == "playing",
            "volumePercent": max(0, min(100, volume_percent)),
            "title": title,
            "artist": artist,
            "progressPercent": max(0, min(100, progress)),
        })
    finally:
        await asyncio.gather(*atv.close(), return_exceptions=True)


async def command_serve(args):
    atv = await connect_atv(args)
    emit({"event": "ready"})
    try:
        for line in sys.stdin:
            line = line.strip()
            if not line:
                continue
            try:
                request = json.loads(line)
                command = request.get("command")
                if command == "volume":
                    delta = int(request.get("delta", 0))
                    for _ in range(min(abs(delta), 20)):
                        if delta > 0:
                            await atv.audio.volume_up()
                        elif delta < 0:
                            await atv.audio.volume_down()
                    emit({"event": "ok", "command": "volume", "volume": getattr(atv.audio, "volume", None)})
                elif command == "remote":
                    remote_command = getattr(atv.remote_control, request.get("name", "play_pause"))
                    await remote_command()
                    emit({"event": "ok", "command": "remote"})
                elif command == "stop":
                    emit({"event": "stopping"})
                    break
                else:
                    emit({"event": "error", "message": f"unknown command: {command}"})
            except Exception as exc:
                emit({"event": "error", "message": str(exc)})
    finally:
        await asyncio.gather(*atv.close(), return_exceptions=True)


async def main_async(args):
    if args.command == "scan":
        await command_scan(args)
    elif args.command == "pair":
        await command_pair(args)
    elif args.command == "volume":
        await command_volume(args)
    elif args.command == "remote":
        await command_remote(args)
    elif args.command == "state":
        await command_state(args)
    elif args.command == "serve":
        await command_serve(args)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--storage", required=True)
    subparsers = parser.add_subparsers(dest="command", required=True)

    scan_parser = subparsers.add_parser("scan")
    scan_parser.add_argument("--timeout", type=int, default=5)

    pair_parser = subparsers.add_parser("pair")
    pair_parser.add_argument("--identifier", required=True)
    pair_parser.add_argument("--protocol", required=True)

    volume_parser = subparsers.add_parser("volume")
    volume_parser.add_argument("--identifier", required=True)
    volume_parser.add_argument("--delta", required=True)

    remote_parser = subparsers.add_parser("remote")
    remote_parser.add_argument("--identifier", required=True)
    remote_parser.add_argument("--command", required=True)

    state_parser = subparsers.add_parser("state")
    state_parser.add_argument("--identifier", required=True)

    serve_parser = subparsers.add_parser("serve")
    serve_parser.add_argument("--identifier", required=True)

    args = parser.parse_args()
    try:
        asyncio.run(main_async(args))
    except Exception as exc:
        print(str(exc), file=sys.stderr)
        raise


if __name__ == "__main__":
    main()
