#!/usr/bin/env python3
"""Makes the renderers' shaders from src/shaders/quad.vert and quad.frag:

  src/shaders_gl.inc   the GLSL source as C strings, with "#version 330 core" (render_gl.c compiles it at run time)
  src/shaders_vk.inc   SPIR-V as C arrays, compiled with glslc as "#version 450" (which defines VULKAN) for render_vk.c

Both are committed, so building gpuic needs neither Python nor glslc. Each carries a hash of the sources it was made
from; --check (a ctest) says whether they are still what the sources give, without glslc: the GL strings are compared,
the SPIR-V by its hash (another glslc gives other bytes for the same source)."""
import hashlib
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "src", "shaders")
GL_OUT = os.path.join(ROOT, "src", "shaders_gl.inc")
VK_OUT = os.path.join(ROOT, "src", "shaders_vk.inc")
STAGES = (("vert", "vs"), ("frag", "fs"))


def read(stage):
    with open(os.path.join(SRC, "quad." + stage), encoding="utf-8") as f:
        return f.read()


def source_hash():
    h = hashlib.sha256()
    for stage, _ in STAGES:
        h.update(read(stage).encode())
    return h.hexdigest()[:16]


def c_string(text):
    lines = []
    for line in text.splitlines(keepends=True):
        lines.append('    "' + line.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n") + '"')
    return "\n".join(lines)


def gl_inc():
    out = ["/* Made by tools/gen_shaders.py from src/shaders/quad.vert and quad.frag (sources %s): do not edit. */" % source_hash()]
    for stage, name in STAGES:
        out.append("static const char* %s_src =\n%s;" % (name, c_string("#version 330 core\n" + read(stage))))
    return "\n".join(out) + "\n"


def spirv(stage):
    with tempfile.TemporaryDirectory() as d:
        src = os.path.join(d, "quad." + stage)
        with open(src, "w", encoding="utf-8") as f:
            f.write("#version 450\n" + read(stage))
        spv = os.path.join(d, "quad.spv")
        subprocess.run(["glslc", "--target-env=vulkan1.3", "-fauto-map-locations", "-O", "-o", spv, src], check=True)
        with open(spv, "rb") as f:
            data = f.read()
    words = [int.from_bytes(data[i:i + 4], "little") for i in range(0, len(data), 4)]
    rows = []
    for i in range(0, len(words), 8):
        rows.append("    " + ", ".join("0x%08x" % w for w in words[i:i + 8]) + ",")
    return "\n".join(rows)


def vk_inc():
    out = ["/* Made by tools/gen_shaders.py from src/shaders/quad.vert and quad.frag (sources %s): do not edit. */" % source_hash()]
    for stage, name in STAGES:
        out.append("static const uint32_t %s_spv[] = {\n%s\n};" % (name, spirv(stage)))
    return "\n".join(out) + "\n"


def main():
    if "--check" in sys.argv:
        stale = []
        with open(GL_OUT, encoding="utf-8") as f:
            if f.read() != gl_inc():
                stale.append(GL_OUT)
        with open(VK_OUT, encoding="utf-8") as f:
            if "(sources %s)" % source_hash() not in f.readline():
                stale.append(VK_OUT)
        for path in stale:
            print("stale: %s (run tools/gen_shaders.py)" % os.path.relpath(path, ROOT))
        return 1 if stale else 0
    with open(GL_OUT, "w", encoding="utf-8") as f:
        f.write(gl_inc())
    with open(VK_OUT, "w", encoding="utf-8") as f:
        f.write(vk_inc())
    return 0


if __name__ == "__main__":
    sys.exit(main())
