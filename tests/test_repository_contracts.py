"""Fast, non-destructive repository checks for safety-critical contracts."""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]
SKETCH = (ROOT / "DigitalLibrarian.ino").read_text(encoding="utf-8")
STORAGE = (ROOT / "Storage.cpp").read_text(encoding="utf-8")
UI = (ROOT / "UIManager.cpp").read_text(encoding="utf-8")
WORKER = (ROOT / "BackgroundWorker.cpp").read_text(encoding="utf-8")
NETWORK = (ROOT / "NetworkManager.cpp").read_text(encoding="utf-8")
MEDIA = (ROOT / "MediaManager.cpp").read_text(encoding="utf-8")


class RepositoryContracts(unittest.TestCase):
    def test_mutating_routes_are_post_only(self):
        for route in ("/api/control", "/api/setcover", "/api/lookup", "/restart"):
            self.assertIn(f'server.on("{route}", HTTP_POST', SKETCH)

    def test_destructive_storage_tests_are_compile_time_gated(self):
        self.assertIn("ENABLE_DESTRUCTIVE_STORAGE_TESTS", SKETCH)
        self.assertIn('server.on("/api/tests/run", HTTP_POST', SKETCH)

    def test_generated_pages_do_not_put_pin_in_urls(self):
        self.assertNotIn("?pin=", SKETCH)
        self.assertNotIn("searchParams.set('pin'", SKETCH)

    def test_auth_cookie_does_not_contain_the_pin(self):
        self.assertIn('"DL_AUTH=" + webSessionToken', SKETCH)
        self.assertNotIn('"DL_AUTH=" + web_pin', SKETCH)
        self.assertIn("webRequestAuthorized(bool allowFormPin = false)", SKETCH)

    def test_storage_writes_use_temporary_files(self):
        self.assertIn('String tmpPath = path + ".tmp";', STORAGE)
        self.assertIn("replaceFileWithRollback", STORAGE)

    def test_detail_updates_roll_back_when_index_save_fails(self):
        self.assertGreaterEqual(STORAGE.count("rollbackReplacedFile(path, hadDetail)"), 3)
        self.assertIn("IndexVector updated = vec", STORAGE)

    def test_track_summary_sd_work_runs_in_background_worker(self):
        self.assertIn("case JOB_TRACK_SUMMARY_LOAD", WORKER)
        summary_scheduler = UI[UI.index("static void schedule_track_summary_load"):]
        summary_scheduler = summary_scheduler[:summary_scheduler.index("// Helper functions")]
        self.assertNotIn("Storage.loadTracklist", summary_scheduler)

    def test_web_random_action_takes_lvgl_lock(self):
        random_action = SKETCH[SKETCH.index('if (action == "random")'):]
        random_action = random_action[:random_action.index('} else if (action == "select")')]
        self.assertIn("lvgl_port_lock", random_action)
        self.assertIn("lvgl_port_unlock", random_action)

    def test_cover_reads_are_verified(self):
        self.assertIn("bytesRead != jpg_size", UI)
        self.assertIn("MALLOC_CAP_SPIRAM", UI)

    def test_https_clients_verify_certificates(self):
        self.assertNotIn("setInsecure()", NETWORK)
        self.assertNotIn("setInsecure()", MEDIA)
        trust = (ROOT / "TlsTrust.h").read_text(encoding="utf-8")
        self.assertIn("setCACertBundle", trust)
        self.assertIn("configTime", trust)

    def test_led_count_has_hard_bounds(self):
        globals_header = (ROOT / "AppGlobals.h").read_text(encoding="utf-8")
        self.assertRegex(globals_header, r"LED_MIN_COUNT\s*=\s*1")
        self.assertRegex(globals_header, r"LED_MAX_COUNT\s*=\s*1200")


if __name__ == "__main__":
    unittest.main()
