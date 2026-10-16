# Northstar API reference provenance

The API references below come from the official [R2Northstar/NorthstarLauncher](https://github.com/R2Northstar/NorthstarLauncher) project, covered by the accompanying MIT `LICENSE`. The minimal source archive retains only this provenance file and the license, rather than the full reference snapshot. The plugin uses a small ABI declaration in `src/northstar_abi.hpp` and an independently implemented entry point; no reference SDK source is required to compile it.

Reference commit: `9a1402c5019657d86a64ad7092740a97f76f0986`.

| Upstream reference filename | Original path in that commit |
| --- | --- |
| `IPluginId.h` | `primedev/plugins/interfaces/IPluginId.h` |
| `IPluginCallbacks.h` | `primedev/plugins/interfaces/IPluginCallbacks.h` |
| `ISys.h` | `primedev/plugins/interfaces/sys/ISys.h` |
| `interface.cpp` | `primedev/plugins/interfaces/interface.cpp` |
| `plugins.cpp` | `primedev/plugins/plugins.cpp` |
| `pluginmanager.cpp` | `primedev/plugins/pluginmanager.cpp` |
| `vscript.h` | `primedev/vscript/vscript.h` |
| `vsquirrel.h` | `primedev/vscript/languages/squirrel_re/vsquirrel.h` |
| `squirrel_api.h` | `primedev/vscript/languages/squirrel_re/include/squirrel.h` |
| `sqobject.h` | `primedev/vscript/languages/squirrel_re/squirrel/sqobject.h` |
| `sqvm.h` | `primedev/vscript/languages/squirrel_re/squirrel/sqvm.h` |
| `squirrelclasstypes.h` | `primedev/squirrel/squirrelclasstypes.h` |
| `squirrel.cpp` | `primedev/squirrel/squirrel.cpp` |
| `LICENSE` | `LICENSE` |

The source snapshot identifies the interface names as `PluginId001`, `PluginCallbacks001`, and `NSSys001`. Some prose in the documentation adds an `I` prefix to the first two; the executable interface lookup uses the names without that prefix.

Northstar's `ADD_SQFUNC` macro and `SquirrelManager` are internal, not exported plugin services. This implementation uses the official VM lifecycle callbacks, registration ABI, and the engine entry points documented by Northstar's own implementation. It installs no detours or binary patches. Registration and string access are allowed only after checking the supported game binaries' complete SHA-256 hashes, PE identities, executable sections, and function bytes. An existing registration jump is accepted only if its bounded jump chain leads to the Northstar module.

The checked local game files are:

The server save-load bootstrap also uses the official `sq_getfunction` entry at
`server.dll + 0x6c80` and `CallScriptInitCallback` at `server.dll + 0x1d5c0`, both
listed in the upstream `squirrel.cpp` at the reference commit. The same full-file identity and entry-byte
checks apply. Bootstrap runs only from `RunFrame` on the game thread, after its
script function becomes available and the client has sent its first script event,
and never holds the VM lock across script calls. Save restore can replace the
wrapped SQVM without a new creation callback: bootstrap and coroutine ownership
checks re-read the current root from the wrapper and reject null shared state.

An earlier late-loading experiment used the official initialization-only
`compilefile` declaration phase, but the live restore dump showed the global
declaration registry at shared state offset `0x40a0` is null after restore.
The plugin removes these calls and has no runtime script compiler dependency.
When server exports are absent, the fresh CLIENT VM observes movement state.
Existing server exports can still be initialized without loading new files.

| File | SHA-256 |
| --- | --- |
| `bin/x64_retail/client.dll` | `002b36487fec7c98882929fbccdb506d0146bbf3270cbb964a4d21c5edf7eebc` |
| `server.dll` | `a5ca3a25c8ae56952a26141b0f6cdcb6c19086c8c39013f7cb345d5d723661af` |

Official documentation:

- [Native plugin installation and interfaces](https://docs.northstar.tf/Modding/plugins/)
- [Required plugin callbacks](https://docs.northstar.tf/Modding/plugins/required-interfaces/)
- [Northstar system interface](https://docs.northstar.tf/Modding/plugins/exposed-interfaces/northstar/)
- [Squirrel native function conventions](https://docs.northstar.tf/Modding/squirrel/cpp_api/sq_functions/)
