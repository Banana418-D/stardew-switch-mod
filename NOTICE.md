# Third-party notices

## This mod

Author: **Banana** -- <https://github.com/lisooreal0202-commits/stardew-switch-mod>

Redistributing this package is fine.  Keep this file, `LICENSE`, the author
line and the source link with it: the shipped `subsdk8` links GPLv2 code, so
the licence and the source have to travel with the binary.  Removing them --
or republishing under another name -- is a licence violation, not a "repost".

## exlaunch -- GPLv2

This mod is built on [exlaunch](https://github.com/shadowninja108/exlaunch)
by shadowninja108, which is licensed under the GNU General Public License v2.
The full licence text is in `LICENSE`. Because the shipped `subsdk8` links
that code, the corresponding source is in this repository, under `source/`.
GPLv2 requires it to be available to whoever receives the binary, so it is
kept next to the thing it builds rather than behind a separate download.

## Stardew Valley

Stardew Valley is (c) ConcernedApe. This package contains **no** game code and
exactly two files derived from retail game data: `romfs/Content/Animals/horse.xnb`,
the optional tractor appearance, and `romfs/Content/Data/Minecarts.xnb`, the
minecart destination table (the vanilla file plus ten entries, so the carts can
reach the volcano, the desert and the rest). The horse sheet's top half is the
vanilla horse sheet, pixel for pixel, and Minecarts.xnb starts from the vanilla
table -- those files are game data and are not ours to license; they are shipped
because the game can only load a replacement as a whole, and each can be removed
by deleting that one file without affecting anything else. Everything else in
the package is code that runs alongside the game the user already owns.

## Tractor Mod -- Pathoschild (MIT)

The **optional** tractor appearance download (a separate release asset, not part
of this package) reuses the tractor sprite sheet from Pathoschild's Tractor Mod.
Only the tractor rows are his art; the rows above them are the vanilla horse
sheet and are not ours to license either -- which is exactly why that file is
distributed on its own and is never committed to the repository.

```
MIT License

Copyright (c) Pathoschild

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## NPCMapLocations -- Bouhm (MIT)

`source/program/npc_crop_table.hpp` reproduces the 38 per-NPC head-crop offsets
from NPCMapLocations' `ModConstants.NpcMarkerOffsets`, so villagers look on
Switch.  They are data rather than code, but they are
Bouhm's tuned values, so they are copied verbatim under the MIT licence and this
notice travels with them.  Three further entries (Bouncer / Henchman / MrQi)
were measured for this project.  No code from that mod is included.

```
MIT License

Copyright (c) 2019 Brian Pak (Bouhm)

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## Tesla overlay -- libtesla (GPLv2) and Atmosphere dmntcht (GPLv2)

`switch/.overlays/sdv_items.ovl` is our own program, but it is built against two
third-party GPLv2 components that are not included in this package:

  libtesla   https://github.com/WerWolv/libtesla        the Tesla menu framework
  dmntcht    Atmosphere-NX (Copyright 2018-2019)        the dmnt:cht service wrapper

Its own source is in `overlay-src/` (main.cpp and the three data headers), as
GPLv2 requires when the binary is distributed.  The two components above are
available from their upstream projects.
