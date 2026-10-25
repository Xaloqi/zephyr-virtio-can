#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Pass/fail gate for a twister run, from twister.json (not from the exit code alone).

  twister_gate.py twister.json --rc N --suites S [--cases C] [--first-failure FILE]

Passes only if twister exited 0 AND exactly S test configurations ran and all passed
AND (when given) exactly C test cases ran and all passed. Zero tests is a failure.
"""
import argparse, collections, json, sys

ap = argparse.ArgumentParser()
ap.add_argument("json")
ap.add_argument("--rc", type=int, required=True, help="twister's exit status")
ap.add_argument("--suites", type=int, required=True)
ap.add_argument("--cases", type=int)
ap.add_argument("--first-failure")
ap.add_argument("--build-only", action="store_true", help="twister reports built-but-not-run configurations as 'not run'; count those as passed")
a = ap.parse_args()
OK = ("passed", "not run") if a.build_only else ("passed",)

errors, first = [], ""
try:
    d = json.load(open(a.json))
except (OSError, ValueError) as e:
    print(f"FAIL: no usable {a.json}: {e}")
    sys.exit(1)

suites = collections.Counter()
cases = collections.Counter()
for ts in d.get("testsuites", []):
    suites[ts.get("status")] += 1
    for tc in ts.get("testcases", []):
        cases[tc.get("status")] += 1
        if tc.get("status") != "passed" and not first:
            first = f'{ts.get("name")} [{ts.get("platform")}] {tc.get("identifier")}: {tc.get("status")} {tc.get("reason") or ""}'
    if ts.get("status") not in OK and not first:
        first = f'{ts.get("name")} [{ts.get("platform")}]: {ts.get("status")} {ts.get("reason") or ""}'

print(f"twister exit {a.rc}; configurations {dict(suites)}; test cases {dict(cases)}")
if a.rc != 0:
    errors.append(f"twister exited {a.rc}")
good = sum(suites[k] for k in OK)
if sum(suites.values()) != a.suites or good != a.suites:
    errors.append(f"expected {a.suites} configurations passed, got {good} of {sum(suites.values())}")
if a.cases is not None and (sum(cases.values()) != a.cases or cases["passed"] != a.cases):
    errors.append(f"expected {a.cases} test cases passed, got {cases['passed']} of {sum(cases.values())}")
if a.first_failure:
    open(a.first_failure, "a").write(first + "\n" if first else "")
if errors:
    print("FAIL: " + "; ".join(errors))
    sys.exit(1)
print(f"PASS: {a.suites} configurations" + (f", {a.cases} test cases" if a.cases is not None else "") + " passed")
