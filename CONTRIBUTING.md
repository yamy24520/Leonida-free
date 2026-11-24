# Contributing to Leonida-free

To contribute a fix, documentation, or a test, make your changes on a branch and open a pull request. No payment is required. Describe the problem, what you expected, and what you actually observed. For a regression, include a synthetic test case if possible.

Use English for issues, pull requests, and public announcements so contributors can follow the discussion.

1. Before changing the code, reproduce the behavior with an input created for the test. Do not use game files or a proprietary SDK in an example.
2. Keep the change focused and add a test. Preserve the attribution, licenses, and notices for any work you reuse. Distinguish a CPU validation result from behavior observed on a GPU or a frame actually shown on screen.
3. Run the relevant test from the repository root. For example, these tests need neither the game nor a GPU:
   ```sh
   python -I -B tests/test_dds_png.py
   python -I -B tests/test_dds_png_dx10.py
   ```
4. Stage only the files you intend to publish, then check them:
   ```sh
   git diff --cached --stat
   git diff --cached --check
   python -I scripts/check_no_game_data.py
   ```
   The last check recognizes file formats; it cannot establish a file’s license or origin. Review text files manually, verify redistribution rights, and check for sensitive information.

Do not attach game binaries, archives, content extracted or translated from the game, screenshots of the game, keys, tokens, personal file paths, session logs, or proprietary SDKs. Share aggregate counts and synthetic test fixtures that you have the right to distribute instead.
