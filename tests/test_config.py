import base64
import copy
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import types
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "server/gameserver"))
spec = importlib.util.spec_from_file_location("bbsc_runner", ROOT / "server/run.py")
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)
import lantern_gameserver

class ConfigurationTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.path = Path(self.directory.name) / "config.json"
        self.config = json.loads((ROOT / "server/config.example.json").read_text(encoding="utf-8"))
        self.config["registrationKey"] = "test_fixture_not_a_real_secret_123"
        self.config["publicAddress"] = "127.0.0.1"
        self.config["ports"] = dict(shadnet=41313, webApi=41315, gameApi=40443,
                                    relay=40444, udp=41314, stun=43478, bbhost=48671)

    def load(self):
        self.path.write_text(json.dumps(self.config), encoding="utf-8")
        return runner.load_config(self.path)

    def test_custom_ports_reach_both_services(self):
        config = self.load()
        command, native = runner.plan(config)
        for key, option in (("webApi", "--shadnet-webapi-port"), ("gameApi", "--port"),
                            ("relay", "--relay-port"), ("stun", "--bbhost-stun-port"),
                            ("bbhost", "--bbhost-port")):
            self.assertEqual(command[command.index(option) + 1], str(config["ports"][key]))
        self.assertIn("UnsecuredPort=41313\n", native)
        self.assertIn("MatchingUdpPort=41314\n", native)
        self.assertIn("WebApiPort=41315\n", native)
        self.assertNotIn(config["registrationKey"], " ".join(command))

    def test_generated_status_uses_custom_webapi_port(self):
        old = lantern_gameserver.server.ARGS
        self.addCleanup(setattr, lantern_gameserver.server, "ARGS", old)
        lantern_gameserver.server.ARGS = types.SimpleNamespace(
            shadnet="127.0.0.1", shadnet_webapi_port=41315, port=40443,
            advertise="127.0.0.1", host="127.0.0.1", summon_interval=5,
            level_range=0, no_playlog=True)
        body = base64.b64decode(lantern_gameserver.build_ss_info(quiet=True)).decode()
        self.assertIn("http://127.0.0.1:41315", body)
        self.assertIn("http://127.0.0.1:40443", body)
        self.assertNotIn(":31315", body)
        self.assertIn("<gameurl4>", body)

    def test_invalid_secret_and_ports_rejected(self):
        original = copy.deepcopy(self.config)
        for value in (0, 65536, True, "31313"):
            self.config = copy.deepcopy(original)
            self.config["ports"]["shadnet"] = value
            with self.assertRaises(ValueError):
                self.load()
        self.config = original
        self.config["registrationKey"] = "x\nAdminsList=attacker"
        with self.assertRaises(ValueError):
            self.load()

    def test_duplicate_listeners_rejected(self):
        self.config["ports"]["gameApi"] = self.config["ports"]["webApi"]
        with self.assertRaises(ValueError):
            self.load()

    def test_paths_relative_to_config_not_current_directory(self):
        config = self.load()
        self.assertEqual(Path(config["paths"]["runtime"]), self.path.parent / "runtime")

    def test_generated_files_preserve_existing_data(self):
        config = self.load()
        _, native = runner.plan(config)
        runner.prepare(config, native)
        runtime = Path(config["paths"]["runtime"])
        database = runtime / "game/online-data.json"
        database.write_text('{"existing": true}')
        runner.prepare(config, native)
        self.assertEqual(database.read_text(), '{"existing": true}')
        self.assertEqual((runtime / "game/server-notice.txt").read_text(encoding="utf-8"), config["notice"])

    def test_check_does_not_start_or_write_runtime(self):
        config = self.load()
        result = subprocess.run([sys.executable, str(ROOT / "server/run.py"), "--config", str(self.path), "--check"],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn(config["registrationKey"], result.stdout + result.stderr)
        self.assertFalse(Path(config["paths"]["runtime"]).exists())

    def test_bbhost_disabled_has_no_extra_listeners(self):
        self.config["bbhost"]["enabled"] = False
        command, _ = runner.plan(self.load())
        self.assertIn("--no-bbhost", command)
        self.assertNotIn("--bbhost-port", command)

if __name__ == "__main__":
    unittest.main()
