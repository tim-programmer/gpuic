#!/usr/bin/env python3
"""Generates include/gpuic/gpuic_chain.inc, the chained style methods of gpuic::Element, from the C header.

Every `gp_node *gp_name(args);` modifier in gpuic.h becomes `name(args)` (so gp_flex_row() is .flex_row(), gp_gap(8) is .gap(8)),
once for a named Element (returns Element&) and once for a temporary (returns Element&&). What needs more than a pass-through (handlers
that take functions, gradients that take arrays, the variadic constructors) is written by hand in gpuic.hpp and listed in HAND below.
The Tailwind-style scale that GPUI has (gap_2, p_4, w_1_2, rounded_md, text_lg, shadow_md ...) is generated here too.

    tools/gen_cpp_chain.py            writes the file
    tools/gen_cpp_chain.py --check    exits 1 if the file on disk is not what the header would give (a ctest does this)
"""
import os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HEADER = os.path.join(ROOT, "include/gpuic/gpuic.h")
OUT = os.path.join(ROOT, "include/gpuic/gpuic_chain.inc")
OUT_TEST = os.path.join(ROOT, "tests/cpp_passthrough.inc")

# written by hand in gpuic.hpp (function-pointer parameters, arrays, variadics, elements rather than modifiers)
HAND = {
    "with_animation", "children", "mods", "child_view", "child_view_cached", "div_", "text_", "img_", "svg_img_", "group_",
    "uniform_list_", "list_",
    "bg_linear_gradient", "bg_radial_gradient", "bg_repeating_gradient", "bg_repeating_radial_gradient", "bg_conic_gradient",
    "border_linear_gradient", "border_radial_gradient", "border_conic_gradient", "stroke_gradient_stops", "stroke_dash",
    "on_action", "on_action_data", "on_key_up", "on_key_up_data", "on_key_down", "on_key_down_data", "on_text_input",
    "on_text_input_data", "on_click", "on_click_data", "on_mouse_down", "on_mouse_down_data", "on_drag", "on_mouse_up",
    "on_layout", "on_ime", "on_scroll", "on_wheel", "on_wheel_data", "list_sticky", "hover_style", "active_style", "focus_style", "mask", "backdrop_filter",
}
KEYWORDS = {"and", "or", "not", "xor", "new", "delete", "default", "private", "public", "template", "this", "typename", "union", "register"}

def parse():
    src = open(HEADER).read()
    out = []
    for m in re.finditer(r"^gp_node \*gp_(\w+)\(([^)]*)\);", src, re.M):
        name, params = m.group(1), m.group(2).strip()
        if name in HAND or name.endswith("_") or "..." in params:
            continue
        if re.search(r"_fn\b|\*\*|_stop", params):
            raise SystemExit("gen_cpp_chain: %s takes a function or an array: write it by hand and add it to HAND" % name)
        if name in KEYWORDS:
            raise SystemExit("gen_cpp_chain: %s is a C++ keyword" % name)
        plist = [] if params in ("", "void") else [p.strip() for p in params.split(",")]
        out.append((name, plist))
    return out

def split_param(p):
    m = re.match(r"^(.*?)(\w+)$", p.strip())
    return m.group(1).strip(), m.group(2)

def fl(x):
    """A float literal: 8 -> 8.0f, 0.5 -> 0.5f."""
    t = "%.9g" % x
    return (t if ("." in t or "e" in t) else t + ".0") + "f"

def method(name, plist, call):
    """Both qualifiers of one chained method; `call` is the C expression building the modifier from the parameter names."""
    decl = []
    for p in plist:
        t, n = split_param(p)
        t = t.replace("const char *", "Str").replace("const char*", "Str")
        decl.append((t if t.endswith("Str") else t, n))
    sig = ", ".join("%s %s" % (t, n) for t, n in decl)
    return ("    Element& %s(%s) & { return add(%s); }\n"
            "    Element&& %s(%s) && { return std::move(add(%s)); }\n") % (name, sig, call, name, sig, call)

# Tailwind spacing: n * 4 px, with the half and fractional steps
SCALE = [("0", 0), ("px", 1), ("0p5", 2), ("1", 4), ("1p5", 6), ("2", 8), ("2p5", 10), ("3", 12), ("3p5", 14), ("4", 16), ("5", 20),
         ("6", 24), ("7", 28), ("8", 32), ("9", 36), ("10", 40), ("11", 44), ("12", 48), ("14", 56), ("16", 64), ("20", 80),
         ("24", 96), ("28", 112), ("32", 128), ("36", 144), ("40", 160), ("44", 176), ("48", 192), ("52", 208), ("56", 224),
         ("60", 240), ("64", 256), ("72", 288), ("80", 320), ("96", 384)]
SPACING = ["gap", "p", "px", "py", "pt", "pr", "pb", "pl", "m", "mx", "my", "mt", "mr", "mb", "ml",
           "top", "right", "bottom", "left", "inset", "w", "h", "min_w", "min_h", "max_w", "max_h"]
FRACTIONS = [("1_2", 0.5), ("1_3", 1 / 3), ("2_3", 2 / 3), ("1_4", 0.25), ("3_4", 0.75), ("1_5", 0.2), ("2_5", 0.4), ("3_5", 0.6), ("4_5", 0.8)]
ROUNDED = [("none", 0), ("sm", 2), ("md", 6), ("lg", 8), ("xl", 12), ("2xl", 16), ("3xl", 24)]
TEXT = [("xs", 12), ("sm", 14), ("base", 16), ("lg", 18), ("xl", 20), ("2xl", 24), ("3xl", 30), ("4xl", 36)]
WEIGHTS = [("thin", 100), ("extralight", 200), ("light", 300), ("medium", 500), ("semibold", 600), ("extrabold", 800), ("black", 900)]
SHADOWS = [("sm", 0, 1, 3, 0.10), ("md", 0, 4, 8, 0.14), ("lg", 0, 10, 16, 0.16), ("xl", 0, 20, 25, 0.18), ("2xl", 0, 25, 50, 0.25)]

def generate():
    lines = ["// Generated by tools/gen_cpp_chain.py from gpuic.h: do not edit. Included inside the body of gpuic::Element.\n"]
    have = set()
    lines.append("    // ---- every modifier of the C header, by its C name without the gp_ prefix\n")
    for name, plist in parse():
        names = [split_param(p)[1] for p in plist]
        call = "gp_%s(%s)" % (name, ", ".join(("%s.c_str()" % n if split_param(p)[0].replace(" ", "").endswith("char*") else n) for p, n in zip(plist, names)))
        lines.append(method(name, plist, call))
        have.add(name)
    def emit(name, call, params=""):
        if name in have:
            return
        have.add(name)
        plist = [p for p in params.split(",") if p.strip()]
        lines.append(method(name, plist, call))
    lines.append("\n    // ---- GPUI / Tailwind spellings: spacing in steps of 4 px (gap_2 is 8 px, p_4 is 16 px, w_px is 1 px)\n")
    for prefix in SPACING:
        cfn = {"w": "gp_w", "h": "gp_h", "min_w": "gp_min_w", "min_h": "gp_min_h", "max_w": "gp_max_w", "max_h": "gp_max_h"}.get(prefix, "gp_" + prefix)
        for suffix, px in SCALE:
            emit("%s_%s" % (prefix, suffix), "%s(%s)" % (cfn, fl(px)))
        if prefix in ("w", "h", "min_w", "min_h", "max_w", "max_h"):
            frac = {"w": "gp_w_frac", "h": "gp_h_frac"}.get(prefix)
            if frac:
                for suffix, f in FRACTIONS:
                    emit("%s_%s" % (prefix, suffix), "%s(%s)" % (frac, fl(f)))
    emit("size", "gp_size_px(px)", "float px")
    for suffix, px in SCALE:
        emit("size_%s" % suffix, "gp_size_px(%s)" % fl(px))
    lines.append("\n    // ---- corners, borders, type, shadows\n")
    for suffix, px in ROUNDED:
        emit("rounded_%s" % suffix, "gp_rounded(%s)" % fl(px))
    for side, fn in (("", "gp_border"), ("t", "gp_border_top"), ("r", "gp_border_right"), ("b", "gp_border_bottom"), ("l", "gp_border_left"),
                     ("x", "gp_border_x"), ("y", "gp_border_y")):
        for w in (0, 1, 2, 4, 8):
            emit("border_%s%s" % (side + "_" if side else "", w), "%s(%s)" % (fn, fl(w)))
    for suffix, px in TEXT:
        emit("text_%s" % suffix, "gp_text_size(%s)" % fl(px))
    for suffix, w in WEIGHTS:
        emit("font_%s" % suffix, "gp_font_weight(%d)" % w)
    for suffix, dx, dy, blur, a in SHADOWS:
        emit("shadow_%s" % suffix, "gp_shadow(%s, %s, %s, gp_rgba(0x000000%02x))" % (fl(dx), fl(dy), fl(blur), round(a * 255)))
    emit("shadow_none", "gp_shadow(0.0f, 0.0f, 0.0f, gp_rgba(0))")
    emit("relative", "(gp_node *)nullptr")      # elements are positioned relative to their parent unless gp_absolute
    emit("flex", "(gp_node *)nullptr")          # every div is a flex container already
    emit("whitespace_normal", "(gp_node *)nullptr")   # (the default)
    return "".join(lines)

def sample(ptype, i):
    """A value of that C type for the pass-through test; each argument of a call gets a different one, so swapped parameters show."""
    t = ptype.replace(" ", "")
    if t == "float":
        return "%sf" % (0.25 * (i + 1) + 0.125)
    if t == "int":
        return str(2 + i)
    if t == "gp_color":
        return "gp_rgb(0x%06x)" % (0x102030 + 0x0b0c0d * i)
    if t == "uint64_t":
        return "%dull" % (1000003 * (i + 1))
    if t in ("gp_scroll", "gp_focus"):
        return "%s{ %du }" % (ptype, 7 + i)
    if t.endswith("char*"):
        return '"ab,cd%d"' % i
    raise SystemExit("gen_cpp_chain: no sample value for a parameter of type %s: add one in sample()" % ptype)

def generate_test():
    """tests/cpp_passthrough.inc: for every generated pass-through method, the call on a C++ element and the C function on a C element."""
    lines = ["// Generated by tools/gen_cpp_chain.py: do not edit. One row per C modifier: the chained method and the C function, same arguments.\n"]
    for name, plist in parse():
        args = [sample(split_param(p)[0], i) for i, p in enumerate(plist)]
        a = ", ".join(args)
        lines.append('{ "%s", [] { return div().%s(%s); }, [] { return gp_div(gp_%s(%s)); } },\n' % (name, name, a, name, a))   # on a temporary
        lines.append('{ "%s (named)", [] { Element e = div(); e.%s(%s); return e; }, [] { return gp_div(gp_%s(%s)); } },\n' % (name, name, a, name, a))   # on a named element
    return "".join(lines)

if __name__ == "__main__":
    outputs = [(OUT, generate()), (OUT_TEST, generate_test())]
    if "--check" in sys.argv:
        for path, text in outputs:
            cur = open(path).read() if os.path.exists(path) else ""
            if cur != text:
                sys.stderr.write("%s is out of date: run tools/gen_cpp_chain.py\n" % os.path.basename(path))
                sys.exit(1)
        sys.exit(0)
    for path, text in outputs:
        open(path, "w").write(text)
        print("wrote %s (%d lines)" % (path, text.count("\n")))
