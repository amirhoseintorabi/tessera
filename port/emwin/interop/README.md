# emWin interoperability declarations

`tessera_emwin_api.h` here is **not** a SEGGER header, is not derived from one,
and contains no emWin code. It is hand-written declarations of the ~30
functions and types that `../tessera_emwin.c` calls, written from the published
API so that the binding can be **compiled and exercised** rather than only
written.

Nothing in this repository is named `GUI.h` or `WM.h`. Those names belong to
SEGGER, and a file of ours carrying one would be confusing at best whatever the
contents.

`emwin_probe.c` implements them as a recorder: a minimal window manager —
handles, extra bytes, callbacks, timers — with drawing calls that count instead
of draw.

emWin itself is commercially licensed by SEGGER, and nothing here substitutes
for a licence.

## Why this exists

Without it, `tessera_emwin.c` would be a file nobody had ever built. With it,
CI compiles the binding under the same
`-Wall -Wextra -Wconversion -Wsign-conversion -Werror` as the rest of the
library, and `tests/test_ports.c` creates the window, sends it `WM_PAINT`,
`WM_TOUCH`, `WM_TIMER` and `WM_DELETE`, and checks what it did:

- one bitmap drawn per loaded tile, one placeholder per missing one
- drawing offset correctly by the window origin
- memory devices created and destroyed in pairs, never more than two alive
- drag panning that follows the finger, and stops when the finger lifts
- the timer restarting but *not* invalidating when nothing has changed
- the timer released on `WM_DELETE`

## What it cannot tell you

**Whether emWin behaves the way the binding assumes.** Its clipping, its
memory-device rotation, its bitmap-stream parsing, the exact semantics of
`WM_CF_MEMDEV` and the message ordering around `WM_TOUCH` are all real emWin
behaviour that a recorder cannot stand in for.

So: the port's own logic is verified, the integration is not. Read
[`../../../docs/EMWIN.md`](../../../docs/EMWIN.md) before putting this on a
display for the first time — it lists what to check on hardware.

## Building against real emWin

Point the build at a licensed emWin. `tessera_emwin.h` then includes `"GUI.h"`
and `"WM.h"` by their own names and nothing here is compiled at all:

```sh
cmake -B build -DTESSERA_EMWIN_DIR=/path/to/emWin/Include
```

Setting that path is all it takes: the build stops adding this directory to the
include path and stops defining `TESSERA_EMWIN_INTEROP`, so the binding falls
back to `#include "GUI.h"` and `#include "WM.h"`.
