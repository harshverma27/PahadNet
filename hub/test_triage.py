"""Checks the triage rules with Gemma stubbed out. Run: python3 test_triage.py"""
import hub


def inc(type_, people, note=""):
    return {"type": type_, "people": people, "note": note,
            "type_name": hub.TYPES[type_], "people_name": hub.PEOPLE[people]}


hub.ask_gemma = lambda i: {"english": "x", "summary": "s", "danger": "neck" in i["note"], "danger_reason": "neck"}

assert hub.triage(inc(5, 1))["priority"] == "medium"                   # food, no note: rules only
assert hub.triage(inc(5, 4))["priority"] == "high"                     # 10+ people bumps one level
assert hub.triage(inc(2, 1, "paani neck tak"))["priority"] == "critical"  # danger in note bumps
assert hub.triage(inc(3, 4, "neck"))["priority"] == "critical"         # never above critical
assert hub.triage(inc(1, 1, "ok"))["team"] == "Ambulance"


def down(i):
    raise OSError("connection refused")


hub.ask_gemma = down
r = hub.triage(inc(4, 2, "help"))
assert r["priority"] == "critical" and "error" in r                    # Ollama down: rules still decide
print("triage ok")
