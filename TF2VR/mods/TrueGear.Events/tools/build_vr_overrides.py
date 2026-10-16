"""Generate VR observation and source markers from installed Cockpit scripts.

Upstream scripts are read locally and never bundled with this tool.  Run again
after a VR update; --check detects stale sources and edited generated files.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import sys
import tempfile

from vpk_sources import VpkSourceError, read_reviewed_entry


SCHEMA = "truegear.vr-overrides/1"
MANIFEST = "vr_overrides.manifest.json"
SOURCE_MOD = "Titanfall2VR.Cockpit"
FILES = ("tf2vr_pickups.gnut", "tf2vr_eye_case.gnut", "weapons/_vortex.nut", "weapons/_particle_wall.gnut", "weapons/mp_titanweapon_heat_shield.nut", "weapons/mp_titanability_basic_block.nut", "weapons/mp_titanweapon_vortex_shield.nut")
LEGACY_FILES = FILES[:2]
SOURCE_MODS = {FILES[3]: "Northstar.CustomServers"}
SCRIPT_DIR = Path("mod/scripts/vscripts")
VPK_INDEX = Path("vpk/englishclient_mp_common.bsp.pak000_dir.vpk")
VPK_EXTRACTOR = Path("tools/vendor/tf2-vpk2tar.exe")
VPK_SOURCE_SHA256 = "c9ce697da624c685159943bc50a1bcf4166479becccaeed67556cf2c121071b8"
VPK_SOURCE_CRC32 = "cecb83cb"
VPK_INDEX_SHA256 = "fc5236a2861817426ad94a4d215dc6e76ed6e7565946960989025592b302cec5"
VPK_REFLECT_SOURCE_SHA256 = "4f2a8546f738eeb557cc661c3a54a94fdce257400326f71d18a62a2b0bda7d91"
VPK_REFLECT_SOURCE_CRC32 = "b21032e6"
PICKUP_SIGNATURE = (
    "void function TF2VR_UpdatePickups( int mask, vector left, vector right, "
    "vector leftForward, vector rightForward, vector leftHip, vector rightHip, "
    "vector hipForward )"
)


class OverrideError(ValueError):
    """The installation cannot be updated safely."""


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _hook_function(text: str, signature: str, anchor: str | None, hook: str,
                   *, before_anchor: bool = False, at_end: bool = False) -> str:
    """Require one exact function header and one anchor inside its body."""
    pattern = re.compile(r"(?m)^" + re.escape(signature) + r"\r?\n\{\r?\n")
    matches = list(pattern.finditer(text))
    if len(matches) != 1:
        raise OverrideError(f"Expected one compatible function: {signature}")
    match = matches[0]
    # Supported functions use the upstream convention: the closing
    # function brace is at column zero; nested braces are indented.
    end = re.search(r"(?m)^\}(?=\r?$)", text[match.end():])
    if end is None:
        raise OverrideError(f"Function closing brace missing: {signature}")
    body_end = match.end() + end.start()
    newline = "\r\n" if match.group().endswith("\r\n") else "\n"
    position = match.end()
    if anchor is not None:
        body = text[match.end():body_end]
        anchor_pattern = re.compile(r"(?m)^" + re.escape(anchor) + r"\r?\n")
        anchors = list(anchor_pattern.finditer(body))
        if len(anchors) != 1:
            raise OverrideError(f"Expected one compatible state assignment in {signature}")
        position += anchors[0].start() if before_anchor else anchors[0].end()
    if at_end:
        position = body_end
    indentation = re.match(r"[\t ]*", anchor)[0] if anchor is not None and not at_end else "\t"
    inserted = newline.join(indentation + line for line in hook.splitlines()) + newline
    return text[:position] + inserted + text[position:]


def _guarded(call: str) -> str:
    return (
        "try\n{\n\t" + call + "\n}\ncatch ( error )\n{\n"
        '\t// Keep observation failures isolated and silent.\n}'
    )


def _require_server_guard(text: str, anchor: str, *, allow_client_else: bool = False) -> None:
    """Authoritative absorption hooks may not move into a predicted branch."""
    branches: list[str] = []
    found = 0
    for line in text.splitlines():
        directive = line.lstrip("\ufeff").strip()
        if directive.startswith("#if "):
            branches.append(directive[4:].strip())
        elif directive.startswith("#elseif "):
            if not branches:
                raise OverrideError("Unexpected shield preprocessor branch")
            branches[-1] = directive[8:].strip()
        elif directive == "#else":
            if not branches:
                raise OverrideError("Unexpected shield preprocessor branch")
            branches[-1] = "!" + branches[-1] if branches[-1] in ("CLIENT", "SERVER") else "else"
        elif directive.startswith("#endif"):
            if not branches:
                raise OverrideError("Unexpected shield preprocessor ending")
            branches.pop()
        if line == anchor:
            found += 1
            authoritative = "SERVER" in branches or (allow_client_else and "!CLIENT" in branches)
            if not authoritative or "CLIENT" in branches:
                raise OverrideError("Shield impact anchor is no longer SERVER-only")
    if found != 1:
        raise OverrideError("Expected one compatible shield impact anchor")


def _function_body(text: str, signature: str) -> str:
    pattern = re.compile(r"(?m)^" + re.escape(signature) + r"\r?\n\{\r?\n")
    matches = list(pattern.finditer(text))
    if len(matches) != 1:
        raise OverrideError(f"Expected one compatible function: {signature}")
    start = matches[0].end()
    end = re.search(r"(?m)^\}(?=\r?$)", text[start:])
    if end is None:
        raise OverrideError(f"Function closing brace missing: {signature}")
    return text[start:start + end.start()]


def _hook_vortex_refires(text: str) -> str:
    """Observe completed fire calls without changing the game's success values.

    DoVortexAttackForImpactData/totalfired are not proof of a successful spawn:
    three projectile helpers return true even when the entity is null. Observe
    their successful entity branches instead. The ReflectAttack hitscan branch
    only queues a signal, so it deliberately has no firing observation here.
    """
    bullets = "int function Vortex_FireBackBullets( entity vortexWeapon, WeaponPrimaryAttackParams attackParams )"
    body = _function_body(text, bullets).replace("\r\n", "\n")
    fire = (
        "\t\tif ( bulletCount == 1 )\n"
        "\t\t\tvortexWeapon.FireWeaponBullet( attackParams.pos, attackParams.dir, bulletCount, damageType )\n"
        "\t\telse\n"
        "\t\t\tShotgunBlast( vortexWeapon, attackParams.pos, attackParams.dir, bulletCount, damageType )\n"
        "\t}\n"
    )
    if (body.count(fire) != 1 or body.count("\treturn bulletCount\n") != 1
            or body.index(fire) > body.index("\treturn bulletCount\n")):
        raise OverrideError("Vortex completed bullet firing branches changed")
    text = _hook_function(
        text, bullets, "\treturn bulletCount",
        "#if SERVER && SP\nif ( bulletCount > 0 )\n{\n"
        + _guarded("TrueGear_OnTitanVortexReflected( vortexWeapon, bulletCount, 0 )")
        + "\n}\n#endif", before_anchor=True)

    helpers = (
        ("bool function Vortex_FireBackExplosiveRound( vortexWeapon, attackParams, impactData, sequenceID )", "bolt", "FireWeaponBolt"),
        ("bool function Vortex_FireBackProjectileBullet( vortexWeapon, attackParams, impactData, sequenceID )", "bolt", "FireWeaponBolt"),
        ("bool function Vortex_FireBackRocket( vortexWeapon, attackParams, impactData, sequenceID )", "rocket", "FireWeaponMissile"),
        ("bool function Vortex_FireBackGrenade( entity vortexWeapon, attackParams, impactData, int attackSeedCount, float baseFuseTime )", "grenade", "FireWeaponGrenade"),
    )
    for signature, projectile, firing_api in helpers:
        _require_server_guard(text, signature)
        body = _function_body(text, signature).replace("\r\n", "\n")
        success = "\tif ( " + projectile + " )\n\t{\n"
        setup = "\t\tVortex_ProjectileCommonSetup( " + projectile + ", impactData )"
        if (body.count(success) != 1 or body.count(setup + "\n") != 1
                or body.count("vortexWeapon." + firing_api + "(") != 1):
            raise OverrideError("Vortex projectile success branch changed")
        branch_start = body.index(success) + len(success)
        branch_end = body.find("\n\t}", branch_start)
        if not (body.index("vortexWeapon." + firing_api + "(") < branch_start
                <= body.index(setup) < branch_end):
            raise OverrideError("Vortex projectile observation must follow a successful spawn")
        text = _hook_function(
            text, signature, setup,
            "#if SP\n" + _guarded("TrueGear_OnTitanVortexReflected( vortexWeapon, 0, 1 )") + "\n#endif")
    return text


def _require_heat_acceptance(text: str, signature: str, anchor: str, validation: str) -> None:
    body = _function_body(text, signature)
    _require_server_guard(body, anchor, allow_client_else=True)
    normalized = body.replace("\r\n", "\n")
    client = "\t#if CLIENT\n\t\treturn true\n\t#else\n"
    rejection = validation + "\n\t\t\treturn false\n"
    if (normalized.count(client) != 1 or normalized.count(rejection) != 1
            or normalized.count(anchor + "\n") != 1):
        raise OverrideError("Heat shield impact acceptance branches changed")
    position = normalized.index(anchor + "\n")
    if not (normalized.index(client) < normalized.index(rejection) < position):
        raise OverrideError("Heat shield impact must follow successful validation")
    if len(re.findall(r"(?m)^\s*return true$", normalized[position:])) != 2:
        raise OverrideError("Heat shield successful impact returns changed")


def _hook_absorbed_returns(text: str, signature: str, hook: str) -> str:
    """Observe each successful absorb, including the two unbraced early exits.

    Requiring all three exact success returns keeps new upstream accept/reject
    branches from silently becoming haptic events. Failed absorbs are untouched.
    """
    header = re.compile(r"(?m)^" + re.escape(signature) + r"\r?\n\{\r?\n")
    matches = list(header.finditer(text))
    if len(matches) != 1:
        raise OverrideError(f"Expected one compatible function: {signature}")
    match = matches[0]
    end = re.search(r"(?m)^\}(?=\r?$)", text[match.end():])
    if end is None:
        raise OverrideError(f"Function closing brace missing: {signature}")
    body_end = match.end() + end.start()
    body = text[match.end():body_end]
    returns = re.compile(r"(?m)^([\t ]*)return true(?=\r?$)")
    if len(returns.findall(body)) != 3:
        raise OverrideError("Expected three compatible successful vortex absorption returns")
    newline = "\r\n" if match.group().endswith("\r\n") else "\n"
    def replace_return(found: re.Match) -> str:
        indentation = found[1]
        return newline.join([indentation + "{"]
                            + [indentation + "\t" + line for line in hook.splitlines()]
                            + [indentation + "\treturn true", indentation + "}"])
    body = returns.sub(replace_return, body)
    return text[:match.end()] + body + text[body_end:]


def _replace_pulses(text: str, signature: str, *, grabbed: bool, expected: int) -> str:
    """Replace only exact pulses in a verified function; reject changed branches."""
    pattern = re.compile(r"(?m)^" + re.escape(signature) + r"\r?\n\{\r?\n")
    functions = list(pattern.finditer(text))
    if len(functions) != 1:
        raise OverrideError(f"Expected one compatible function: {signature}")
    start = functions[0].end()
    end = re.search(r"(?m)^\}(?=\r?$)", text[start:])
    if end is None:
        raise OverrideError(f"Function closing brace missing: {signature}")
    stop = start + end.start()
    body = text[start:stop]
    pulse = re.compile(r"(?m)^([\t ]*)TF2VR_PulseHands\( 1 << side \)(?=\r?$)")
    if len(pulse.findall(body)) != expected or len(re.findall(r"\bTF2VR_PulseHands\s*\(", body)) != expected:
        raise OverrideError(f"Expected {expected} compatible item pulse(s) in {signature}")
    replacement = "TrueGear_ItemGripPulse( 1 << side, " + ("true" if grabbed else "false") + " )"
    body = pulse.sub(lambda match: match[1] + replacement, body)
    return text[:start] + body + text[stop:]


def transform(filename: str, raw: bytes) -> bytes:
    """Preserve the original BOM/newlines and insert only the agreed calls."""
    try:
        text = raw.decode("utf-8")
    except UnicodeDecodeError as error:
        raise OverrideError(f"{filename}: expected UTF-8 script") from error
    if any(hook in text for hook in ("TrueGear_ObserveHandPoses", "TrueGear_ObserveItemGrip", "TrueGear_ItemGripPulse", "TrueGear_ObserveTitanShield", "TrueGear_OnTitanShieldImpact", "TrueGear_OnTitanParticleWallCreated", "TrueGear_OnTitanParticleWallImpact", "TrueGear_OnTitanVortexReflected")):
        raise OverrideError(f"{filename}: source already contains TrueGear hooks")
    if filename == FILES[0]:
        # Native reads pickup kinds immediately AFTER this callback returns.
        # Observe after both sides have updated their convars, before the final
        # highlight loop. A callback-entry hook would report the previous frame.
        text = _hook_function(text, PICKUP_SIGNATURE,
                              "\tforeach ( entity weapon in file.weapons )",
                              _guarded("TrueGear_ObserveHandPoses( left, right, leftHip, rightHip )"),
                              before_anchor=True)
    elif filename == FILES[1]:
        grab = "void function TF2VR_EyeCaseGrab( entity player, int side, int target, vector hand )"
        release = "void function TF2VR_EyeCaseRelease( entity player, int side )"
        # These three pulses follow confirmed SERE item actions. Drawer opening
        # in EyeCaseFollow has no matching item effect and must remain original.
        text = _replace_pulses(text, grab, grabbed=True, expected=1)
        text = _replace_pulses(text, release, grabbed=False, expected=2)
        text = _hook_function(
            text,
            grab,
            "\tfile.grabs[side] = target",
            _guarded("TrueGear_ObserveItemGrip( player, side, target, true )"),
        )
        text = _hook_function(
            text,
            release,
            "\tfile.grabs[side] = TF2VR_EYE_CASE_NONE",
            _guarded("TrueGear_ObserveItemGrip( player, side, grab, false )"),
        )
    elif filename == FILES[2]:
        text = _hook_vortex_refires(text)
        # Only successful authoritative interception emits a shield impact.
        # Raising/lowering a shield and CLIENT prediction emit no feedback.
        absorb = "bool function TryVortexAbsorb( entity vortexSphere, entity attacker, vector origin, int damageSourceID, entity weapon, string weaponName, string impactType, entity projectile = null, damageType = null, reflect = false )"
        _require_server_guard(text, absorb)
        impact = 'TrueGear_OnTitanShieldImpact( owner, vortexWeapon.GetWeaponClassName() == "mp_titanweapon_heat_shield" ? "heat" : "vortex", vortexWeapon, attacker, origin )'
        text = _hook_absorbed_returns(text, absorb, "#if SP\n" + _guarded(impact) + "\n#endif")
        electric = "function Vortex_HandleElectricDamage( entity ent, entity attacker, damage, entity weapon )"
        _require_server_guard(text, electric)
        text = _hook_function(
            text,
            electric,
            "\treturn 0",
            '#if SP\n' + _guarded('TrueGear_OnTitanShieldImpact( ent, vortexWeapon.GetWeaponClassName() == "mp_titanweapon_heat_shield" ? "heat" : "vortex", vortexWeapon, attacker, vortexSphere.GetOrigin() )') + '\n#endif',
            before_anchor=True,
        )
        bullet = "bool function CodeCallback_OnVortexHitBullet( entity weapon, entity vortexSphere, var damageInfo )"
        projectile = "bool function CodeCallback_OnVortexHitProjectile( entity weapon, entity vortexSphere, entity attacker, entity projectile, vector contactPos )"
        bullet_anchor = "\t\tdamage = max( 0.0, damage )"
        projectile_anchor = "\t\tdamage = damage + CalculateTitanSniperExtraDamage( projectile, vortexSphere )"
        _require_server_guard(_function_body(text, bullet), bullet_anchor, allow_client_else=True)
        _require_server_guard(_function_body(text, projectile), projectile_anchor, allow_client_else=True)
        text = _hook_function(
            text,
            bullet,
            bullet_anchor,
            "#if SERVER && SP\nif ( !isAmpedWall )\n{\n" + _guarded('TrueGear_OnTitanParticleWallImpact( vortexSphere, "hitscan", DamageInfo_GetAttacker( damageInfo ), DamageInfo_GetDamagePosition( damageInfo ) )') + "\n}\n#endif",
        )
        text = _hook_function(
            text,
            projectile,
            projectile_anchor,
            "#if SERVER && SP\nif ( !isAmpedWall )\n{\n" + _guarded('TrueGear_OnTitanParticleWallImpact( vortexSphere, "projectile", attacker, contactPos )') + "\n}\n#endif",
        )
    elif filename == FILES[3]:
        text = _hook_function(
            text,
            "void function CreateParticleWallForOwnerFromDirection( entity weaponOwner, float duration, vector dir )",
            '\t\tvortexSphere.SetParent( groundEntity, "", true, 0 )',
            "#if SERVER && SP\n" + _guarded("TrueGear_OnTitanParticleWallCreated( weaponOwner, vortexSphere )") + "\n#endif",
            at_end=True,
        )
    elif filename == FILES[4]:
        for signature, anchor, position, validation in (
            ("bool function OnWeaponVortexHitBullet_titanweapon_heat_shield( entity weapon, entity vortexSphere, var damageInfo )",
             "\t\tVortexDrainedByImpact( weapon, attackerWeapon, null, null )", "origin", "\t\tif ( !ValidateVortexImpact( vortexSphere ) )"),
            ("bool function OnWeaponVortexHitProjectile_titanweapon_heat_shield( entity weapon, entity vortexSphere, entity attacker, entity projectile, vector contactPos )",
             "\t\tVortexDrainedByImpact( weapon, projectile, projectile, null )", "contactPos", "\t\tif ( !ValidateVortexImpact( vortexSphere, projectile ) )"),
        ):
            # This drain follows ValidateVortexImpact and precedes BOTH success
            # returns. The CLIENT branch's unconditional true is not an impact.
            _require_heat_acceptance(text, signature, anchor, validation)
            text = _hook_function(
                text, signature, anchor,
                "#if SERVER && SP\n" + _guarded('TrueGear_OnTitanShieldImpact( weapon.GetWeaponOwner(), "heat", weapon, attacker, ' + position + ' )') + "\n#endif",
            )
    elif filename == FILES[5]:
        signature = "void function BasicBlock_OnDamage( entity blockingEnt, var damageInfo )"
        _require_server_guard(text, signature)
        text = _hook_function(
            text, signature, "\tDamageInfo_ScaleDamage( damageInfo, damageScale )",
            "#if SP\nif ( DamageInfo_GetDamage( damageInfo ) > 0.0 )\n{\n" + _guarded('TrueGear_OnTitanShieldImpact( blockingEnt, "sword", blockingEnt.GetOffhandWeapon( OFFHAND_LEFT ), DamageInfo_GetAttacker( damageInfo ), DamageInfo_GetDamagePosition( damageInfo ) )') + "\n}\n#endif",
        )
    elif filename == FILES[6]:
        # Amplified reflection queues a signal in VortexReflectAttack. Observe
        # its actual bullet consumer after the blast, before clearing the count.
        signature = "function AmpedVortexRefireThink( entity weapon )"
        body = _function_body(text, signature).replace("\r\n", "\n")
        fire = "\t\t\tShotgunBlast( weapon, weaponOwner.EyePosition(), weaponOwner.GetPlayerOrNPCViewVector(), expect int( weapon.s.ampedBulletCount ), damageTypes.shotgun | DF_VORTEX_REFIRE )"
        successful = ('\t\tweapon.WaitSignal( "FireAmpedVortexBullet" )\n\n'
                      '\t\tif ( IsValid( weaponOwner )\t)\n\t\t{\n'
                      + fire + '\n\t\t\tweapon.s.ampedBulletCount = 0\n\t\t}\n')
        _require_server_guard(text, "\t\t\tthread AmpedVortexRefireThink( weapon )")
        if body.count(successful) != 1:
            raise OverrideError("Amped vortex completed bullet firing branch changed")
        text = _hook_function(
            text, signature, fire,
            "#if SERVER && SP\nif ( weapon.s.ampedBulletCount > 0 )\n{\n"
            + _guarded("TrueGear_OnTitanVortexReflected( weapon, expect int( weapon.s.ampedBulletCount ), 0 )")
            + "\n}\n#endif")
    else:
        raise OverrideError(f"Unsupported override filename: {filename}")
    return text.encode("utf-8")


def _output(mod_root: Path, filename: str) -> Path:
    if filename not in FILES:
        raise OverrideError(f"Unexpected manifest filename: {filename}")
    path = mod_root / SCRIPT_DIR / filename
    if not path.resolve().is_relative_to(mod_root.resolve()):
        raise OverrideError(f"Output escapes the mod directory: {path}")
    return path


def _manifest_path(mod_root: Path) -> Path:
    path = mod_root / MANIFEST
    if path.is_symlink() or not path.resolve().is_relative_to(mod_root.resolve()):
        raise OverrideError("Manifest must be a regular file inside the mod directory")
    return path


def source_relative(filename: str) -> Path:
    if filename in FILES[5:]:
        return Path("@game") / (VPK_INDEX.as_posix() + "!") / _vpk_source_spec(filename)[0]
    return Path(SOURCE_MODS.get(filename, SOURCE_MOD)) / SCRIPT_DIR / filename


def _vpk_source_spec(filename: str) -> tuple[str, str, str]:
    if filename == FILES[5]:
        return "scripts/vscripts/" + filename, VPK_SOURCE_SHA256, VPK_SOURCE_CRC32
    if filename == FILES[6]:
        return "scripts/vscripts/" + filename, VPK_REFLECT_SOURCE_SHA256, VPK_REFLECT_SOURCE_CRC32
    raise OverrideError(f"Unexpected VPK source filename: {filename}")


def source_path(mod_root: Path, filename: str) -> Path:
    """Resolve only fixed catalog sources, never a path from the manifest."""
    if filename not in FILES:
        raise OverrideError(f"Unexpected source filename: {filename}")
    if filename not in FILES[5:]:
        return mod_root.parent / source_relative(filename)
    mod_root = mod_root.resolve()
    if (mod_root.name != "TrueGear.Events" or mod_root.parent.name != "mods"
            or mod_root.parent.parent.name != "TF2VR"):
        raise OverrideError("VPK source requires the known game/TF2VR/mods/TrueGear.Events layout")
    game_root = mod_root.parents[2]
    source = game_root / VPK_INDEX
    if not source.resolve().is_relative_to(game_root):
        raise OverrideError("VPK source escapes the game directory")
    return source


def _source_data(mod_root: Path, filename: str, *, extractor_mod_root: Path | None = None) -> tuple[bytes, dict | None]:
    source = source_path(mod_root, filename)
    if filename not in FILES[5:]:
        if not source.is_file():
            raise OverrideError(f"Installed source script not found: {source}")
        return source.read_bytes(), None
    entry, expected_sha, expected_crc = _vpk_source_spec(filename)
    extractor_root = (extractor_mod_root or mod_root).resolve()
    extractor = extractor_root / VPK_EXTRACTOR
    if not extractor.resolve().is_relative_to(extractor_root):
        raise OverrideError("VPK extractor escapes the mod directory")
    try:
        raw, proof = read_reviewed_entry(source, entry, extractor,
                                        expected_index_sha256=VPK_INDEX_SHA256,
                                        expected_sha256=expected_sha, expected_crc32=expected_crc)
        proof.update({"index": VPK_INDEX.as_posix(), "index_sha256": VPK_INDEX_SHA256})
        return raw, proof
    except VpkSourceError as error:
        raise OverrideError(f"VPK source proof failed: {error}") from error


def _load_manifest(mod_root: Path) -> dict | None:
    path = _manifest_path(mod_root)
    if not path.exists():
        return None
    try:
        result = json.loads(path.read_text(encoding="utf-8"))
        records = result["files"]
        if result["schema"] != SCHEMA or set(records) not in (set(FILES), set(FILES[:6]), set(FILES[:5]), set(FILES[:4]), set(FILES[:3]), set(LEGACY_FILES)):
            raise ValueError("unexpected schema or files")
        for filename, entry in records.items():
            accepted_sources = {source_relative(filename).as_posix()}
            if filename in FILES[5:]:
                # Allow upgrading/removing earlier managed installs without
                # requiring the old developer-only extracted source copies.
                accepted_sources.add("@game/TrueGear.Native/devtools/vpk-review-tools/titan-shield-scripts/"
                                     + Path(filename).name)
            if entry["source"] not in accepted_sources:
                raise ValueError("unexpected source path")
            if entry["output"] != (SCRIPT_DIR / filename).as_posix():
                raise ValueError("unexpected output path")
            for key in ("source_sha256", "output_sha256"):
                if not re.fullmatch(r"[a-f0-9]{64}", entry[key]):
                    raise ValueError("invalid hash")
        return result
    except (KeyError, TypeError, ValueError) as error:
        raise OverrideError(f"Invalid {MANIFEST}: {error}") from error


def _verify_existing(mod_root: Path, manifest: dict | None, *, require_all: bool) -> None:
    # Validate every target before any writes or deletions.
    for filename in FILES:
        path = _output(mod_root, filename)
        if path.is_symlink():
            raise OverrideError(f"Refusing a symlink output: {path}")
        if not path.exists():
            if require_all:
                raise OverrideError(f"Generated file is missing: {path}")
            continue
        if manifest is None or filename not in manifest["files"]:
            raise OverrideError(f"Unmanaged override already exists; preserving it: {path}")
        if digest(path.read_bytes()) != manifest["files"][filename]["output_sha256"]:
            raise OverrideError(f"Generated file was edited; preserving it: {path}")


def _atomic_write(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(prefix=".tg-override-", dir=path.parent)
    temporary = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(data)
        os.replace(temporary, path)
    finally:
        if temporary.exists():
            temporary.unlink()


def build(mod_root: Path, *, source_mod_root: Path | None = None) -> dict:
    mod_root = mod_root.resolve()
    source_mod_root = (source_mod_root or mod_root).resolve()
    manifest = _load_manifest(mod_root)
    _verify_existing(mod_root, manifest, require_all=False)
    records = {}
    generated = {}
    for filename in FILES:
        raw, proof = _source_data(source_mod_root, filename, extractor_mod_root=mod_root)
        generated[filename] = transform(filename, raw)
        records[filename] = {
            "source": source_relative(filename).as_posix(),
            "source_sha256": digest(raw),
            "output": (SCRIPT_DIR / filename).as_posix(),
            "output_sha256": digest(generated[filename]),
        }
        if proof is not None:
            records[filename]["source_proof"] = proof
    result = {"schema": SCHEMA, "files": records}
    # No files change until every input, anchor and existing output is checked.
    for filename, data in generated.items():
        _atomic_write(_output(mod_root, filename), data)
    _atomic_write(_manifest_path(mod_root), (json.dumps(result, indent=2) + "\n").encode("utf-8"))
    return result


def check(mod_root: Path, *, source_mod_root: Path | None = None) -> dict:
    mod_root = mod_root.resolve()
    source_mod_root = (source_mod_root or mod_root).resolve()
    manifest = _load_manifest(mod_root)
    if manifest is None:
        raise OverrideError("VR overrides have not been generated; run this tool without --check")
    if set(manifest["files"]) != set(FILES):
        raise OverrideError("Generator added shield observation; rebuild the overrides")
    _verify_existing(mod_root, manifest, require_all=True)
    for filename in FILES:
        raw, proof = _source_data(source_mod_root, filename, extractor_mod_root=mod_root)
        if digest(raw) != manifest["files"][filename]["source_sha256"]:
            raise OverrideError(f"VR source changed or is missing; rebuild the overrides: {source_path(source_mod_root, filename)}")
        if proof != manifest["files"][filename].get("source_proof"):
            raise OverrideError("VPK source proof changed; review and rebuild the overrides")
        expected = transform(filename, raw)
        if digest(expected) != manifest["files"][filename]["output_sha256"]:
            raise OverrideError("Generator changed; rebuild the overrides")
    return manifest


def remove(mod_root: Path) -> None:
    mod_root = mod_root.resolve()
    manifest = _load_manifest(mod_root)
    if manifest is None:
        raise OverrideError("No managed VR overrides manifest exists; nothing was deleted")
    _verify_existing(mod_root, manifest, require_all=False)
    for filename in manifest["files"]:
        path = _output(mod_root, filename)
        if path.exists():
            path.unlink()
    _manifest_path(mod_root).unlink()


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    modes = parser.add_mutually_exclusive_group()
    modes.add_argument("--check", action="store_true", help="verify source version and generated file hashes")
    modes.add_argument("--remove", action="store_true", help="remove only unmodified, manifest-owned overrides")
    arguments = parser.parse_args(argv)
    mod_root = Path(__file__).resolve().parents[1]
    try:
        if arguments.check:
            check(mod_root)
            print("OK: all VR overrides match the installed source mods and generated hashes")
        elif arguments.remove:
            remove(mod_root)
            print("Removed managed VR overrides and manifest; Cockpit originals were preserved")
        else:
            build(mod_root)
            print("Generated 7 local VR/VPK overrides with accepted shield impacts, successful vortex refires and 3 source-marked pulses; engine compilation still needs verification")
        return 0
    except (OverrideError, OSError) as error:
        print(f"VR override error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
