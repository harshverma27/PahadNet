"""Reads JSON lines from the central ESP32 over serial and serves a live dashboard."""
import json
import queue
import sys
import threading
import time
import urllib.request
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

import serial
from serial.tools import list_ports

PORT = sys.argv[1] if len(sys.argv) > 1 else None  # None: auto-detect the central
HTTP_PORT = 8000
HERE = Path(__file__).parent

TYPES = {1: "Injured", 2: "Flood", 3: "Fire", 4: "Trapped", 5: "Food / water",
         6: "Child / elderly", 7: "Other"}
PEOPLE = {1: "1", 2: "2-5", 3: "6-10", 4: "10+"}

MODEL = "gemma3:1b"  # swap to "gemma4:e2b" once pulled: much better Hindi/Hinglish
OLLAMA = "http://127.0.0.1:11434/api/chat"

# Rules own priority and dispatch (must be reliable); Gemma only reads the free-text note.
RULES = {1: ("high", "Ambulance"), 2: ("high", "Boat / rescue team"), 3: ("critical", "Fire brigade"),
         4: ("critical", "Rescue team"), 5: ("medium", "Relief supplies"), 6: ("high", "Rescue team"),
         7: ("medium", "Call back and assess")}
LEVELS = ["medium", "high", "critical"]

NOTE_SCHEMA = {"type": "object", "required": ["english", "summary", "danger", "danger_reason"], "properties": {
    "english": {"type": "string"}, "summary": {"type": "string"},
    "danger": {"type": "boolean"}, "danger_reason": {"type": "string"}}}
NOTE_PROMPT = (
    "You help a disaster control room in India read SOS messages. The message may be Hindi, Hinglish "
    "(Hindi in English letters) or English.\n"
    "english: translate the message to plain English.\n"
    "summary: one short sentence for a rescuer, including how many people.\n"
    "danger: true only if the message mentions something life-threatening (bleeding, broken bone, unconscious, "
    "not breathing, rising water, fire spreading, someone trapped, child or elderly alone).\n"
    "danger_reason: the words that show the danger, or empty.\n"
    'Example: {"type":"Flood","people":"1","message":"paani gale tak aa gaya"} -> '
    '{"english":"Water has reached up to my neck","summary":"One person in neck-deep flood water.",'
    '"danger":true,"danger_reason":"water up to neck"}')

node_info = json.loads((HERE / "nodes.json").read_text())
state = {"nodes": {}, "incidents": [], "serial": "connecting", "model": MODEL}
lock = threading.Lock()
to_analyze = queue.Queue()


def ask_gemma(inc):
    body = {"model": MODEL, "stream": False, "format": NOTE_SCHEMA, "keep_alive": "30m",
            "options": {"temperature": 0},
            "messages": [{"role": "system", "content": NOTE_PROMPT},
                         {"role": "user", "content": json.dumps(
                             {"type": inc["type_name"], "people": inc["people_name"], "message": inc["note"]},
                             ensure_ascii=False)}]}
    req = urllib.request.Request(OLLAMA, json.dumps(body).encode(), {"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=120) as r:
        return json.loads(json.load(r)["message"]["content"])


def triage(inc):
    level, team = RULES.get(inc["type"], RULES[7])
    ai = {"team": team, "model": None, "english": "", "danger_reason": "",
          "summary": f"{inc['type_name']}, {inc['people_name']} people."}
    bump = inc["people"] == 4  # 10+ people
    if inc["note"]:
        t = time.time()
        try:
            out = ask_gemma(inc)
        except Exception as e:  # Ollama down, model missing, bad JSON: rules still give a priority
            ai["error"] = f"Gemma unavailable ({e.__class__.__name__}). Is Ollama running with {MODEL}?"
        else:
            ai.update(model=MODEL, seconds=round(time.time() - t, 1), summary=out["summary"],
                      english=out["english"], danger_reason=out["danger_reason"] if out["danger"] else "")
            bump = bump or out["danger"]
    ai["priority"] = LEVELS[min(LEVELS.index(level) + bump, 2)]
    return ai


def analyze_loop():
    while True:
        inc = to_analyze.get()
        ai = triage(inc)
        with lock:
            inc["ai"] = ai
        print(f"  triage {inc['id']}: {ai['priority']}, {ai['team']}, {ai['summary']!r}", flush=True)


def handle(msg):
    now = time.time()
    node = str(msg.get("node"))
    with lock:
        if msg["kind"] == "beat":
            state["nodes"][node] = {**node_info.get(node, {}), "id": node, "last_seen": now,
                                    "phones": msg["phones"], "rssi": msg["rssi"], "mac": msg["mac"]}
        elif msg["kind"] == "sos":
            inc = {**msg, "time": now, "info": node_info.get(node, {"name": f"Unknown node {node}"}),
                   "type_name": TYPES.get(msg["type"], "?"), "people_name": PEOPLE.get(msg["people"], "?"),
                   "ai": None}
            state["incidents"].insert(0, inc)
            to_analyze.put(inc)
            print(f"SOS from node {node}: {TYPES.get(msg['type'])}, {PEOPLE.get(msg['people'])} people, {msg['note']!r}")


def open_port(port):
    ser = serial.Serial()
    ser.port, ser.baudrate, ser.timeout = port, 115200, 1
    ser.dtr = ser.rts = False  # don't reset the board on open
    ser.open()
    ser.reset_input_buffer()
    return ser


def find_central():
    """Listen on every USB serial port until one sends the central's hello line."""
    while True:
        ports = [PORT] if PORT else [p.device for p in list_ports.comports() if p.vid is not None]
        state["serial"] = "searching " + (", ".join(ports) or "(no USB serial ports)")
        for port in ports:
            try:
                ser = open_port(port)
            except serial.SerialException:
                continue  # busy or gone
            deadline = time.time() + 4  # central says hello every 3 s
            while time.time() < deadline:
                try:
                    line = ser.readline()
                except serial.SerialException:
                    break
                if b'"kind":"central"' in line:
                    print("central node found on", port)
                    return ser, port
            ser.close()
        time.sleep(1)


def serial_loop():
    while True:
        ser, port = find_central()
        state["serial"] = f"connected {port}"
        quiet = 0
        try:
            with ser:
                while quiet < 10:  # no hello for 10 s: board swapped or hung, search again
                    line = ser.readline().decode("utf-8", "replace").strip()
                    quiet = 0 if line else quiet + 1
                    if line.startswith("{"):
                        try:
                            handle(json.loads(line))
                        except (json.JSONDecodeError, KeyError):
                            print("bad line:", line)
        except serial.SerialException as e:
            print("lost central:", e)


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == "/api/state":
            with lock:
                body = json.dumps({**state, "now": time.time()}).encode()
            ctype = "application/json"
        else:
            body = (HERE / "dashboard.html").read_bytes()
            ctype = "text/html; charset=utf-8"
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass


if __name__ == "__main__":
    threading.Thread(target=serial_loop, daemon=True).start()
    threading.Thread(target=analyze_loop, daemon=True).start()
    server = ThreadingHTTPServer(("127.0.0.1", HTTP_PORT), Handler)
    url = f"http://127.0.0.1:{HTTP_PORT}/"
    print(f"Dashboard at {url}, central port: {PORT or 'auto-detect'}")
    webbrowser.open(url)
    server.serve_forever()
