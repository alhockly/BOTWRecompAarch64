"""Turn third_party/smaa/SMAA.hlsl into the C string runtime/nvn/smaa_data.c includes."""
import pathlib
import sys

src, dst = sys.argv[1:]
raw = pathlib.Path(src).read_bytes().replace(b"\r", b"")
raw = bytes(b if b < 128 else ord("?") for b in raw)
lines = []
for line in raw.decode("ascii").split("\n"):
    line = line.replace("\\", "\\\\").replace('"', '\\"')
    lines.append('"' + line + '\\n"')
pathlib.Path(dst).parent.mkdir(parents=True, exist_ok=True)
pathlib.Path(dst).write_text("\n".join(lines) + "\n")
