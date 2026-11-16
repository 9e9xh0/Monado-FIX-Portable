"""Source-level checks for the opt-in --game option: the old positional form must stay, and the install-folder switch
is Windows-only (Linux AppImages/launchers already choose the writable data folder)."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]
MAIN_BOOT = (ROOT / "runtime/host/main_boot.c").read_text(encoding="utf-8")


class GameOption(unittest.TestCase):
    def test_game_option_is_read_only_when_it_is_the_first_argument(self):
        self.assertIn('argc >= 2 && !strcmp(argv[1], "--game")', MAIN_BOOT)

    def test_positional_dump_folder_still_works(self):
        self.assertIn("int first = game_opt ? 2 : 1;", MAIN_BOOT)
        self.assertIn("const char *root = argv[first];", MAIN_BOOT)

    def test_dump_path_is_absolute_before_the_working_folder_changes(self):
        self.assertIn("rt_path_absolute(root, abs_root, sizeof abs_root)", MAIN_BOOT)
        self.assertLess(MAIN_BOOT.index("rt_path_absolute(root"),
                        MAIN_BOOT.index("if (!enter_install_dir())"))

    def test_install_folder_switch_is_windows_only(self):
        start = MAIN_BOOT.index("static bool enter_install_dir(")
        self.assertGreater(start, MAIN_BOOT.rindex("#ifdef _WIN32", 0, start))
        call = MAIN_BOOT.index("if (!enter_install_dir())")
        self.assertGreater(call, MAIN_BOOT.rindex("#ifdef _WIN32", 0, call))


if __name__ == "__main__":
    unittest.main()
