#!/usr/bin/env python3
"""Generate a tiny C++ view of the shared JSON corpus for the host test."""
import json
import pathlib
import sys

source, output = map(pathlib.Path, sys.argv[1:3])
corpus = json.loads(source.read_text(encoding="utf-8"))
management = json.loads(pathlib.Path(sys.argv[3]).read_text(encoding="utf-8")) if len(sys.argv) > 3 else None
shared = json.loads(pathlib.Path(sys.argv[4]).read_text(encoding="utf-8")) if len(sys.argv) > 4 else None

RESULT_NAMES = {
    0: "OK", 1: "BAD_VERSION", 2: "BAD_LENGTH", 3: "BAD_OPCODE",
    4: "BAD_PAYLOAD", 5: "NOT_AUTHORIZED", 6: "BAD_SESSION",
    7: "OLD_SEQUENCE", 8: "SEQUENCE_CONFLICT", 9: "BUSY",
    10: "INTERNAL_ERROR", 11: "NOT_ARMED", 12: "NOT_CONFIGURED",
    13: "SUBSCRIPTION_REQUIRED",
}


def emit_vectors(name, rows):
    lines = [f"inline constexpr Vector {name}[] = {{"]
    for row in rows:
        expected = row.get("expected", "")
        if "expected_result" in row:
            expected = RESULT_NAMES[row["expected_result"]]
        lines.append(f'    {{{json.dumps(row["name"])}, {json.dumps(row["hex"])}, {json.dumps(expected)} }},')
    lines.append("};")
    return lines


parts = ["#pragma once", "struct Vector { const char* name; const char* hex; const char* expected; };"]
parts += emit_vectors("kCommands", corpus["commands"])
parts += emit_vectors("kEvents", corpus["events"])
parts += emit_vectors("kReads", corpus["reads"])
parts += emit_vectors("kRejections", corpus["rejection_cases"])
parts += ["inline constexpr const char* kScenarios[] = {"]
parts += [f'    {json.dumps(row["name"])},' for row in corpus["state_machine_scenarios"]]
parts += ["};"]
if management is not None:
    parts += emit_vectors("kManagementCommands", management["commands"])
    parts += emit_vectors("kManagementEvents", management["events"])
    parts += emit_vectors("kManagementRejections", management["rejection_cases"])
    parts += ["inline constexpr const char* kManagementScenarios[] = {"]
    parts += [f'    {json.dumps(name)},' for name in management["scenarios"]]
    parts += ["};"]
if shared is not None:
    parts += emit_vectors("kSharedReads", shared["reads"])
    parts += emit_vectors("kSharedRejections", shared["rejection_cases"])
    parts += ["inline constexpr const char* kSharedScenarios[] = {"]
    parts += [f'    {json.dumps(name)},' for name in shared["scenarios"]]
    parts += ["};"]
    profile = shared["built_in_profile"]
    parts += [f'inline constexpr const char* kBuiltInProfileId = {json.dumps(profile["id"])};']
    for key, symbol in (("startup", "kBuiltInStartup"), ("logical_minimum", "kBuiltInMinimum"), ("logical_maximum", "kBuiltInMaximum")):
        values = ", ".join(str(int(value)) for value in profile[key])
        parts += [f'inline constexpr int {symbol}[3] = {{{values}}};']
output.write_text("\n".join(parts) + "\n", encoding="utf-8")
