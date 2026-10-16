# Optional VPK reader dependency

The two TrueGear generator source files need tf2-vpk2tar only when regenerating
local game-script overlays. Neither the player pack nor native DLL compilation
requires this tool. The minimal source archive does not bundle its executable,
source snapshot, or unused transitive dependencies.

Reviewed upstream source (MIT):
https://github.com/pg9182/tf2vpk/tree/b73c54d8e4245075ea207c7e51a2a86fdb94e4ff

The commit is recorded in the original GitHub source archive ZIP comment.
Use the upstream go.mod/go.sum and retain upstream dependency licenses when
building or distributing the optional tool.

Reviewed executable SHA-256:
97429c9a0e0791ba818397126d7b05debee5cb2fd081eb5918a97575e3b80220

Build environment: Go 1.24.0, windows/amd64, CGO_ENABLED=0, -trimpath.
From the downloaded upstream source directory, set CGO_ENABLED=0 and run:

    go build -trimpath -buildvcs=false -o tf2-vpk2tar.exe ./cmd/tf2-vpk2tar

Place the output at TF2VR/mods/TrueGear.Events/tools/vendor/tf2-vpk2tar.exe
before running build_vr_overrides.py. The generator deliberately pins the
reviewed executable SHA-256. A different compiler may produce a different hash;
review the result before changing EXTRACTOR_SHA256 in vpk_sources.py.

The tool reads two exact local VPK entries into memory during generation.
