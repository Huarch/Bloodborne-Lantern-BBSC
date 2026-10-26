"""Single configuration entry point for shadNet and the Lantern game service."""
import argparse
import ipaddress
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parent

def load_config(path):
    path = Path(path).resolve()
    config = json.loads(path.read_text(encoding="utf-8-sig"))
    for field in ("publicAddress", "bindAddress"):
        ipaddress.IPv4Address(config[field])
    secret = config["registrationKey"]
    if not isinstance(secret, str) or not re.fullmatch(r"[A-Za-z0-9_-]{20,128}", secret) or secret.startswith("REPLACE_"):
        raise ValueError("registrationKey must be a new 20-128 character URL-safe secret")
    for name in ("shadnet", "webApi", "gameApi", "relay", "udp", "stun", "bbhost"):
        port = config["ports"][name]
        if type(port) is not int or not 1 <= port <= 65535:
            raise ValueError("Invalid port: " + name)
    ports = config["ports"]
    tcp = [ports[n] for n in ("shadnet", "webApi", "gameApi", "relay")]
    if config["bbhost"]["enabled"]:
        tcp.append(ports["bbhost"])
        if ports["udp"] == ports["stun"]:
            raise ValueError("UDP relay and STUN ports must differ")
    if len(tcp) != len(set(tcp)):
        raise ValueError("TCP service ports must differ")
    if not isinstance(config["admins"], list) or any(not re.fullmatch(r"[A-Za-z0-9_-]{3,16}", n) for n in config["admins"]):
        raise ValueError("Invalid admins list")
    if type(config["relaySessionRouting"]) is not bool or type(config["bbhost"]["enabled"]) is not bool:
        raise ValueError("Feature switches must be booleans")
    if type(config["summonInterval"]) is not int or not 1 <= config["summonInterval"] <= 300:
        raise ValueError("Invalid summonInterval")
    match = re.fullmatch(r"(\d+)-(\d+)", config["bbhost"]["relayPorts"])
    if not match or not 1 <= int(match[1]) <= int(match[2]) <= 65535:
        raise ValueError("Invalid bbhost relayPorts")
    if not isinstance(config["bbhost"]["heartbeatTimeout"], (int, float)) or not 5 <= config["bbhost"]["heartbeatTimeout"] <= 300:
        raise ValueError("Invalid heartbeatTimeout")
    if not isinstance(config["notice"], str):
        raise ValueError("notice must be text")
    for name in ("shadnetBinary", "runtime", "qtLibraryPath"):
        value = config["paths"][name]
        if not isinstance(value, str) or (name != "qtLibraryPath" and not value.strip()):
            raise ValueError("Invalid path: " + name)
        if value:
            config["paths"][name] = str((path.parent / value).resolve())
    return config

def plan(config):
    ports = config["ports"]
    runtime = Path(config["paths"]["runtime"])
    command = [sys.executable, "-u", str(ROOT / "gameserver/lantern_gameserver.py"),
               "--shadnet", config["publicAddress"], "--shadnet-webapi-port", str(ports["webApi"]),
               "--host", config["bindAddress"], "--advertise", config["publicAddress"],
               "--port", str(ports["gameApi"]), "--relay-port", str(ports["relay"]),
               "--summon-interval", str(config["summonInterval"]), "--no-playlog",
               "--log-dir", str(runtime / "logs"), "--data", str(runtime / "game/online-data.json")]
    if config["relaySessionRouting"]:
        command.append("--relay-session-routing")
    if config["bbhost"]["enabled"]:
        command += ["--bbhost-port", str(ports["bbhost"]), "--bbhost-stun-port", str(ports["stun"]),
                    "--bbhost-stun-bind", config["bindAddress"], "--bbhost-public-ip", config["publicAddress"],
                    "--bbhost-relay-ports", config["bbhost"]["relayPorts"],
                    "--bbhost-heartbeat-timeout", str(config["bbhost"]["heartbeatTimeout"])]
    else:
        command.append("--no-bbhost")
    native = {"Host": config["bindAddress"], "UnsecuredPort": ports["shadnet"],
              "MatchingUdpPort": ports["udp"], "WebApiPort": ports["webApi"],
              "Matching2Enabled": "true", "BloodborneSeamlessCoop": "true",
              "StatsEnabled": "false", "EmailValidated": "false",
              "AdminsList": ",".join(config["admins"]), "RegistrationSecretKey": config["registrationKey"]}
    return command, "\n".join(f"{key}={value}" for key, value in native.items()) + "\n"

def private_write(path, content):
    path = Path(path)
    temp = path.with_name(path.name + ".tmp")
    descriptor = os.open(temp, os.O_CREAT | os.O_TRUNC | os.O_WRONLY, 0o600)
    with os.fdopen(descriptor, "w", encoding="utf-8", newline="\n") as output:
        output.write(content)
    os.chmod(temp, 0o600)
    os.replace(temp, path)

def prepare(config, native):
    runtime = Path(config["paths"]["runtime"])
    for sub in ("shadnet", "game", "logs"):
        (runtime / sub).mkdir(parents=True, exist_ok=True, mode=0o700)
    private_write(runtime / "shadnet/shadnet.cfg", native)
    private_write(runtime / "game/server-notice.txt", config["notice"])
    for name in ("worlds.cfg", "scoreboards.cfg"):
        target = runtime / "shadnet" / name
        if not target.exists():
            target.write_bytes((ROOT / "shadnet" / name).read_bytes())

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, default=ROOT / "config.json")
    parser.add_argument("--check", action="store_true", help="validate without writing files or starting services")
    args = parser.parse_args()
    try:
        config = load_config(args.config)
        game_command, native = plan(config)
    except (OSError, ValueError, KeyError, TypeError):
        print("Invalid or missing server config.json. Check it against config.example.json; no secret values were logged.", file=sys.stderr)
        return 2
    if args.check:
        print("Server configuration valid; no services started.")
        return 0
    binary = Path(config["paths"]["shadnetBinary"])
    if not binary.is_file():
        print("Build shadnet first and set paths.shadnetBinary.", file=sys.stderr)
        return 2
    os.umask(0o077)
    runtime = Path(config["paths"]["runtime"])
    runtime.mkdir(parents=True, exist_ok=True, mode=0o700)
    # Linux-only server runner: reject a second supervisor for the same database.
    import fcntl
    lock = (runtime / "runner.lock").open("w")
    try:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        print("A server runner is already using this runtime directory.", file=sys.stderr)
        return 2
    prepare(config, native)
    environment = os.environ.copy()
    environment["SHADNET_DATA_DIR"] = str(runtime / "shadnet")
    environment["SHADNET_P2P_RELAY_ADDRESS"] = config["publicAddress"]
    if config["paths"]["qtLibraryPath"]:
        environment["LD_LIBRARY_PATH"] = config["paths"]["qtLibraryPath"] + os.pathsep + environment.get("LD_LIBRARY_PATH", "")
    children = []
    def stop_signal(*_):
        raise KeyboardInterrupt
    signal.signal(signal.SIGTERM, stop_signal)
    try:
        children.append(subprocess.Popen([str(binary)], env=environment))
        children.append(subprocess.Popen(game_command, env=environment))
        while all(child.poll() is None for child in children):
            time.sleep(0.5)
        print("A server component exited; stopping the other component.", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        return 0
    finally:
        for child in children:
            if child.poll() is None:
                child.terminate()
        for child in children:
            try:
                child.wait(timeout=5)
            except subprocess.TimeoutExpired:
                child.kill()
                child.wait()
        lock.close()

if __name__ == "__main__":
    sys.exit(main())
