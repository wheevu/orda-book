#!/usr/bin/env python3
"""Validate the metadata in an orda-book results manifest."""

import json
import pathlib
import re
import sys

SCHEMA = "orda-book/results-manifest/v1"
CAMPAIGN_RE = re.compile(r"^[a-z0-9][a-z0-9-]+$")
SHA40 = re.compile(r"^[0-9a-f]{40}$")
SHA64 = re.compile(r"^[0-9a-f]{64}$")


def validate(path):
    data = json.loads(pathlib.Path(path).read_text())
    assert data.get("schema") == SCHEMA, f"schema must be {SCHEMA}"
    assert CAMPAIGN_RE.fullmatch(data.get("campaign_id", "")), "bad campaign_id"
    assert data.get("experiment_kind") in {
        "pilot",
        "preregistered-campaign",
        "replication",
        "ablation",
        "falsified-hypothesis",
    }
    src = data["source"]
    assert SHA40.fullmatch(src["commit"]), "commit must be 40 hex"
    assert isinstance(src["dirty"], bool)
    assert isinstance(src["dependencies"], dict)
    for key in ["os", "kernel", "cpu", "architecture"]:
        assert key in data["machine"], f"missing machine.{key}"
    assert "compiler" in data["build"] and "build_type" in data["build"]
    execution = data["execution"]
    assert isinstance(execution["argv"], list) and execution["argv"]
    assert isinstance(execution["seeds"], dict)
    assert isinstance(data["artifacts"], list)
    for artifact in data["artifacts"]:
        assert SHA64.fullmatch(artifact["sha256"]), "bad artifact sha256"
        assert isinstance(artifact["bytes"], int) and artifact["bytes"] >= 0
        assert artifact["role"] in {"raw", "summary", "figure", "log", "counterexample"}
    print(f"OK {path}")


if __name__ == "__main__":
    paths = sys.argv[1:] or pathlib.Path(".").rglob("results/manifest.json")
    for manifest in paths:
        validate(manifest)
