"""A fake OpenAI-compatible endpoint, for the behaviour that only shows up over the network.

Two modes, because the two things worth testing here are a working gateway and a broken one:

  full       /models serves a one-entry list; /chat/completions streams a reply
  no-models  /models returns 404; /chat/completions still streams a reply

Binds an ephemeral port and prints "PORT <n>" on stdout once listening, so the caller never has
to guess a port or poll for readiness. Request bodies go to --capture, GET paths to --gets.
"""

import argparse
import json
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer

CHUNKS = [
    {"choices": [{"delta": {"content": "live path works"}}]},
    {
        "choices": [{"delta": {}}],
        "usage": {"prompt_tokens": 9, "completion_tokens": 3, "total_tokens": 12},
    },
]


def handler(args):
    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, *_):
            pass

        def do_GET(self):
            if args.gets:
                with open(args.gets, "a") as f:
                    f.write(self.path + "\n")
            if args.mode == "no-models":
                self.send_response(404)
                self.send_header("content-length", "0")
                self.end_headers()
                return
            body = json.dumps(
                {"data": [{"id": "fake-model", "context_length": 32000}]}
            ).encode()
            self.send_response(200)
            self.send_header("content-type", "application/json")
            self.send_header("content-length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def do_POST(self):
            raw = self.rfile.read(int(self.headers.get("content-length", 0)))
            if args.capture:
                with open(args.capture, "wb") as f:
                    f.write(raw)
            payload = b"".join(
                f"data: {json.dumps(c)}\n\n".encode() for c in CHUNKS
            ) + b"data: [DONE]\n\n"
            self.send_response(200)
            self.send_header("content-type", "text/event-stream")
            self.send_header("content-length", str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)

    return Handler


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--mode", choices=["full", "no-models"], default="full")
    parser.add_argument("--capture")
    parser.add_argument("--gets")
    args = parser.parse_args()

    server = HTTPServer(("127.0.0.1", 0), handler(args))
    print(f"PORT {server.server_address[1]}", flush=True)
    server.serve_forever()


if __name__ == "__main__":
    sys.exit(main())
