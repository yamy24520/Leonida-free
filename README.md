# Leonida-free

Leonida-free is an independent, community-run edition of the Leonida runtime and tools. **Access to our source code and fixes is free, and anyone can contribute without paying or subscribing.** This policy does not change the rights granted by the GPL.

This repository contains the runtime, shader translator, tools, and tests. It **does not include the game**, its assets or keys, a playable executable, or shaders derived from a copy of the game. Passing tests on synthetic data does not establish that the game starts, displays a frame, or is playable. We do not guarantee any of those outcomes.

## Run the tests without the game

With Git and Python 3 installed, run these commands from the repository root. The first checks the tracked files. The next two test DDS image decoding with data generated in memory. None requires the game or a GPU.

```sh
python -I scripts/check_no_game_data.py
python -I -B tests/test_dds_png.py
python -I -B tests/test_dds_png_dx10.py
```

The first check produces no output on success. Each DDS test suite should print the number of tests and `OK`. To contribute, see the [contributing guide](CONTRIBUTING.md).

## Where this edition comes from

On **November 24, 2026**, we started a new Git history with selected sources from [Leonida revision `71d1ab1f`](https://github.com/ASTROWwwW/Leonida/commit/71d1ab1f818c8525e97c62011391b5dd2a3cbee8). The runtime and tests we imported match the public files at that revision. No private test results are included.

The `game/` directory contains only a public manifest of filenames, sizes, and hashes, not the files it identifies. This edition excludes the upstream logo, measurement reports, project tracking records, and proprietary assets. The optional `rpf9-explorer` submodule is a reference to an external repository. Its contents are not included and are not needed for the tests above.

This is an unofficial project. It is not affiliated with Rockstar Games, Take-Two Interactive, or Microsoft. Names and trademarks belong to their respective owners. Leonida code is distributed under the [GNU GPL version 3 or any later version](LICENSE). Third-party components retain their own licenses. See [credits and provenance](CREDITS.md).
