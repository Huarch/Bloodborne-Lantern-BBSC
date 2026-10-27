"""Package the public Windows client without private configuration or player data."""
from pathlib import Path
import argparse
import json
import zipfile

ROOT = Path(__file__).resolve().parent


def package(output: Path, helper: Path, binaries: Path) -> None:
    # Explicit allowlist: never recurse over an installed client or a used staging directory.
    sources = {
        name: ROOT / 'client' / name for name in (
            'PLAY LANTERN + BBSC.cmd', 'CHECK LANTERN + BBSC.cmd',
            'CHECK BBSC UDP.cmd', 'README-zh-CN.txt', 'config.example.json',
            'tools/start-client.ps1', 'tools/client-config.ps1',
            'tools/play-lantern-bbsc.ps1', 'tools/check-bbsc-udp.ps1',
        )
    }
    sources.update({
        'bbsc/bbsc.dll': binaries / 'bbsc.dll',
        'bbsc/bbsc-launch.exe': binaries / 'bbsc-launch.exe',
        'bbsc/mod.json': ROOT / 'emulator/mod/mod.json',
        'bbsc/LICENSE.txt': ROOT / 'LICENSE',
        'bbsc/NOTICE.txt': ROOT / 'NOTICE.md',
        'bbsc/CREDITS.txt': ROOT / 'CREDITS.txt',
        'bbsc/SUPPORTERS.txt': ROOT / 'SUPPORTERS.txt',
        'account-helper/shadnet-sample.exe': helper / 'shadnet-sample.exe',
        'account-helper/LICENSE.txt': helper / 'LICENSE.txt',
        'account-helper/NOTICE.txt': helper / 'NOTICE.txt',
        'account-helper/CREDITS.txt': helper / 'CREDITS.txt',
    })
    missing = [str(path) for path in sources.values() if not path.is_file()]
    if missing:
        raise SystemExit('Missing package inputs:\n' + '\n'.join(missing))
    config_bytes = sources['config.example.json'].read_bytes()
    config = json.loads(config_bytes.decode('utf-8-sig'))
    if (config['serverHost'] != 'example.invalid' or config['registrationKey'] or
            config['gameRoot'] or config['accountFile']):
        raise SystemExit('Public config template contains non-placeholder configuration')
    contents = {name: path.read_bytes() for name, path in sources.items()}
    contents['config.json'] = config_bytes
    prefix = 'Bloodborne-Lantern-BBSC/'
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_suffix(output.suffix + '.tmp')
    with zipfile.ZipFile(temporary, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for name, data in sorted(contents.items()):
            archive.writestr(prefix + name, data)
    with zipfile.ZipFile(temporary) as archive:
        if archive.testzip() is not None:
            raise SystemExit('Archive integrity check failed')
        for name, data in contents.items():
            if archive.read(prefix + name) != data:
                raise SystemExit('Archive content mismatch: ' + name)
    temporary.replace(output)
    print(f'Public client package: {output} ({len(contents)} files, {output.stat().st_size} bytes)')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--account-helper', required=True, type=Path,
                        help='Directory containing the account helper and its license notices')
    parser.add_argument('--binaries', type=Path, default=ROOT / 'build/bbsc-mod')
    parser.add_argument('--output', type=Path,
                        default=ROOT / 'build/releases/Bloodborne-Lantern-BBSC.zip')
    args = parser.parse_args()
    package(args.output.resolve(), args.account_helper.resolve(), args.binaries.resolve())
