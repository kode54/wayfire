#include <wayfire/core.hpp>
#include <cmath>
#include "simple-background.hpp"

wf_cube_simple_background::wf_cube_simple_background()
{}

void wf_cube_simple_background::render_frame(const wf::render_target_t& fb,
    wf_cube_animation_attribs&)
{
    // The cube renders into an EXT_LINEAR-tagged intermediate, so any value written here is
    // interpreted as linear. background_color is a gamma 2.2 sRGB color (the wayfire convention
    // for solid colors, matching wlroots' quad_cm shader). Convert to linear before clearing so
    // the value wlroots' add_texture path sees on the way to the output is actually linear.
    wf::color_t color = background_color;
    auto to_linear = [] (double c)
    {
        return std::pow(std::max(c, 0.0), 2.2);
    };
    color.r = to_linear(color.r);
    color.g = to_linear(color.g);
    color.b = to_linear(color.b);
    OpenGL::clear(color, GL_COLOR_BUFFER_BIT);
}
