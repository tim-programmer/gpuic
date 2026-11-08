/*
 * The header and the C++ layer compile as ISO C++20 under -Wpedantic -Werror (the C compound literals of gpuic.h are braced values in C++, see GP_LIT): every constant and value
 * helper a C++ program is likely to write, in a translation unit of its own. It also runs, to check that the values are what the C spellings mean.
 */
#include <gpuic/gpuic.hpp>

#include <cstdio>

// (made while a view renders: compiled, not run)
[[maybe_unused]] static gpuic::Element made() {
    using namespace gpuic;
    return div().bg(rgb(0x102030)).child(curve(xy(0, 0), xy(1, 1), xy(2, 2), xy(3, 3)).stroke_color(rgb(0xff8000))).child(polyline({ xy(0, 0), xy(5, 5) }));
}

int main() {
    using namespace gpuic;
    gp_color c = gp_rgb(0xff8000);
    gp_color d = gp_rgba(0x00ff0080);
    gp_entity e = GP_ENTITY_NULL;
    gp_image i = GP_IMAGE_NULL;
    gp_svg s = GP_SVG_NULL;
    gp_point p = gp_xy(3, 4);
    gp_color_stop st = gp_stop(c, 0.5f);
    bool ok = c.r == 1.f && d.a > 0.49f && d.a < 0.51f && e.generation == 0 && i.generation == 0 && s.index == 0 && p.y == 4.f && st.at == 0.5f;
    std::printf(ok ? "cpp_pedantic: compiles clean\n" : "cpp_pedantic: unexpected values\n");
    return 0;
}
