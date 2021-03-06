#include <wayfire/plugin.hpp>
#include <wayfire/view.hpp>
#include <wayfire/output.hpp>
#include <wayfire/view-transform.hpp>
#include <wayfire/workspace-stream.hpp>
#include <wayfire/workspace-manager.hpp>
#include <wayfire/signal-definitions.hpp>

#include "blur.hpp"

using blur_algorithm_provider = std::function<nonstd::observer_ptr<wf_blur_base>()>;
class wf_blur_transformer : public wf::view_transformer_t
{
    blur_algorithm_provider provider;
    wf::output_t *output;
    wayfire_view view;

  public:
    wf_blur_transformer(blur_algorithm_provider blur_algorithm_provider,
        wf::output_t *output, wayfire_view view)
    {
        provider     = blur_algorithm_provider;
        this->output = output;
        this->view   = view;
    }

    wf::pointf_t transform_point(wf::geometry_t view,
        wf::pointf_t point) override
    {
        return point;
    }

    wf::pointf_t untransform_point(wf::geometry_t view,
        wf::pointf_t point) override
    {
        return point;
    }

    wlr_box get_bounding_box(wf::geometry_t view, wlr_box region) override
    {
        return region;
    }

    wf::region_t transform_opaque_region(
        wf::geometry_t bbox, wf::region_t region) override
    {
        return region;
    }

    uint32_t get_z_order() override
    {
        return wf::TRANSFORMER_BLUR;
    }

    /* Render without blending */
    void direct_render(wf::texture_t src_tex, wlr_box src_box,
        const wf::region_t& damage, const wf::framebuffer_t& target_fb)
    {
        OpenGL::render_begin(target_fb);
        for (auto& rect : damage)
        {
            target_fb.logic_scissor(wlr_box_from_pixman_box(rect));
            OpenGL::render_texture(src_tex, target_fb, src_box);
        }

        OpenGL::render_end();
    }

    void render_with_damage(wf::texture_t src_tex, wlr_box src_box,
        const wf::region_t& damage, const wf::framebuffer_t& target_fb) override
    {
        wf::region_t clip_damage = damage & src_box;

        /* We want to check if the opaque region completely occludes
         * the bounding box. If this is the case, we can skip blurring
         * altogether and just render the surface. First we disable
         * shrinking and get the opaque region without padding */
        wf::surface_interface_t::set_opaque_shrink_constraint("blur", 0);
        wf::region_t full_opaque = view->get_transformed_opaque_region();

        /* Shrink the opaque region by the padding amount since the render
         * chain expects this, as we have applied padding to damage in
         * frame_pre_paint for this frame already */
        int padding = std::ceil(provider()->calculate_blur_radius() /
            output->render->get_target_framebuffer().scale);
        wf::surface_interface_t::set_opaque_shrink_constraint("blur", padding);

        wf::region_t bbox_region{src_box};
        if ((bbox_region ^ full_opaque).empty())
        {
            /* In case the whole surface is opaque, we can simply skip blurring */
            direct_render(src_tex, src_box, damage, target_fb);

            return;
        }

        wf::region_t opaque_region  = view->get_transformed_opaque_region();
        wf::region_t blurred_region = clip_damage ^ opaque_region;

        provider()->pre_render(src_tex, src_box, blurred_region, target_fb);
        wf::view_transformer_t::render_with_damage(src_tex, src_box, blurred_region,
            target_fb);

        /* Opaque non-blurred regions can be rendered directly without blending */
        direct_render(src_tex, src_box, opaque_region & clip_damage, target_fb);
    }

    void render_box(wf::texture_t src_tex, wlr_box src_box, wlr_box scissor_box,
        const wf::framebuffer_t& target_fb) override
    {
        provider()->render(src_tex, src_box, scissor_box, target_fb);
    }
};

class wayfire_blur : public wf::plugin_interface_t
{
    wf::button_callback button_toggle;

    wf::effect_hook_t frame_pre_paint;
    wf::signal_callback_t workspace_stream_pre, workspace_stream_post,
        view_attached, view_detached;

    const std::string normal_mode = "normal";
    std::string last_mode;

    wf::option_wrapper_t<std::string> method_opt{"blur/method"},
    mode_opt{"blur/mode"};
    wf::option_wrapper_t<wf::buttonbinding_t> toggle_button{"blur/toggle"};
    wf::config::option_base_t::updated_callback_t blur_method_changed, mode_changed;
    std::unique_ptr<wf_blur_base> blur_algorithm;

    const std::string transformer_name = "blur";

    /* the pixels from padded_region */
    wf::framebuffer_base_t saved_pixels;
    wf::region_t padded_region;

    void add_transformer(wayfire_view view)
    {
        if (view->get_transformer(transformer_name))
        {
            return;
        }

        view->add_transformer(std::make_unique<wf_blur_transformer>(
            [=] () {return nonstd::make_observer(blur_algorithm.get()); },
            output, view),
            transformer_name);
    }

    void pop_transformer(wayfire_view view)
    {
        if (view->get_transformer(transformer_name))
        {
            view->pop_transformer(transformer_name);
        }
    }

    void remove_transformers()
    {
        for (auto& view : output->workspace->get_views_in_layer(wf::ALL_LAYERS))
        {
            pop_transformer(view);
        }
    }

    /** Transform region into framebuffer coordinates */
    wf::region_t get_fb_region(const wf::region_t& region,
        const wf::framebuffer_t& fb) const
    {
        wf::region_t result;
        for (const auto& rect : region)
        {
            result |= fb.framebuffer_box_from_geometry_box(
                wlr_box_from_pixman_box(rect));
        }

        return result;
    }

  public:
    void init() override
    {
        grab_interface->name = "blur";
        grab_interface->capabilities = 0;

        blur_method_changed = [=] ()
        {
            blur_algorithm = create_blur_from_name(output, method_opt);
            output->render->damage_whole();
        };
        /* Create initial blur algorithm */
        blur_method_changed();
        method_opt.set_callback(blur_method_changed);

        /* Default mode is normal, which means attach the blur transformer
         * to each view on the output. If on toggle, this means that the user
         * has to manually click on the views they want to blur */
        last_mode    = "none";
        mode_changed = [=] ()
        {
            if (std::string(mode_opt) == last_mode)
            {
                return;
            }

            if (last_mode == normal_mode)
            {
                remove_transformers();
            }

            if (std::string(mode_opt) == normal_mode)
            {
                for (auto& view :
                     output->workspace->get_views_in_layer(wf::ALL_LAYERS))
                {
                    add_transformer(view);
                }
            }

            last_mode = mode_opt;
        };
        mode_changed();
        mode_opt.set_callback(mode_changed);

        /* Toggles the blur state of the view the user clicked on */
        button_toggle = [=] (uint32_t, int, int)
        {
            if (!output->can_activate_plugin(grab_interface))
            {
                return false;
            }

            auto view = wf::get_core().get_cursor_focus_view();
            if (!view)
            {
                return false;
            }

            if (view->get_transformer(transformer_name))
            {
                view->pop_transformer(transformer_name);
            } else
            {
                add_transformer(view);
            }

            return true;
        };
        output->add_button(toggle_button, &button_toggle);

        /* If a view is attached to this output, and we are in normal mode,
         * we should add a blur transformer so it gets blurred
         *
         * Additionally, we don't blur windows in the background layers,
         * as they usually are fully opaque, and there is actually nothing
         * behind them which can be blurred. */
        view_attached = [=] (wf::signal_data_t *data)
        {
            auto view = get_signaled_view(data);
            /* View was just created -> we don't know its layer yet */
            if (!view->is_mapped())
            {
                return;
            }

            if (((std::string)mode_opt == normal_mode) &&
                !(output->workspace->get_view_layer(view) & wf::BELOW_LAYERS))
            {
                if (!view->get_transformer(transformer_name))
                {
                    add_transformer(view);
                }
            }
        };

        /* If a view is detached, we remove its blur transformer.
         * If it is just moved to another output, the blur plugin
         * on the other output will add its own transformer there */
        view_detached = [=] (wf::signal_data_t *data)
        {
            auto view = get_signaled_view(data);
            pop_transformer(view);
        };
        output->connect_signal("view-attached", &view_attached);
        output->connect_signal("view-mapped", &view_attached);
        output->connect_signal("view-detached", &view_detached);

        /* frame_pre_paint is called before each frame has started.
         * It expands the damage by the blur radius.
         * This is needed, because when blurring, the pixels that changed
         * affect a larger area than the really damaged region, e.g the region
         * that comes from client damage */
        frame_pre_paint = [=] ()
        {
            auto damage    = output->render->get_scheduled_damage();
            const auto& fb = output->render->get_target_framebuffer();

            int padding = std::ceil(
                blur_algorithm->calculate_blur_radius() / fb.scale);
            wf::surface_interface_t::set_opaque_shrink_constraint("blur",
                padding);

            wf::region_t padded;
            for (const auto& rect : damage)
            {
                padded |= wlr_box{
                    (rect.x1 - padding),
                    (rect.y1 - padding),
                    (rect.x2 - rect.x1) + 2 * padding,
                    (rect.y2 - rect.y1) + 2 * padding
                };
            }

            output->render->damage(padded);
        };
        output->render->add_effect(&frame_pre_paint, wf::OUTPUT_EFFECT_PRE);

        /* workspace_stream_pre is called before rendering each frame
         * when rendering a workspace. It gives us a chance to pad
         * damage and take a snapshot of the padded area. The padded
         * damage will be used to render the scene as normal. Then
         * workspace_stream_post is called so we can copy the padded
         * pixels back. */
        workspace_stream_pre = [=] (wf::signal_data_t *data)
        {
            auto& damage   = static_cast<wf::stream_signal_t*>(data)->raw_damage;
            const auto& ws = static_cast<wf::stream_signal_t*>(data)->ws;
            const auto& target_fb = static_cast<wf::stream_signal_t*>(data)->fb;

            /* As long as the padding is big enough to cover the
             * furthest sampled pixel by the shader, there should
             * be no visual artifacts. */
            int padding = std::ceil(
                blur_algorithm->calculate_blur_radius() / target_fb.scale);

            wf::region_t expanded_damage;
            for (const auto& rect : damage)
            {
                expanded_damage |= {
                    rect.x1 - padding,
                    rect.y1 - padding,
                    (rect.x2 - rect.x1) + 2 * padding,
                    (rect.y2 - rect.y1) + 2 * padding
                };
            }

            /* Keep rects on screen */
            expanded_damage &= output->render->get_ws_box(ws);

            /* Compute padded region and store result in padded_region.
             * We need to be careful, because core needs to scale the damage
             * back and forth for wlroots. */
            padded_region = get_fb_region(expanded_damage, target_fb) ^
                get_fb_region(damage, target_fb);

            OpenGL::render_begin(target_fb);
            /* Initialize a place to store padded region pixels. */
            saved_pixels.allocate(target_fb.viewport_width,
                target_fb.viewport_height);

            /* Setup framebuffer I/O. target_fb contains the pixels
             * from last frame at this point. We are writing them
             * to saved_pixels, bound as GL_DRAW_FRAMEBUFFER */
            saved_pixels.bind();
            GL_CALL(glBindFramebuffer(GL_READ_FRAMEBUFFER, target_fb.fb));

            /* Copy pixels in padded_region from target_fb to saved_pixels. */
            for (const auto& box : padded_region)
            {
                GL_CALL(glBlitFramebuffer(
                    box.x1, target_fb.viewport_height - box.y2,
                    box.x2, target_fb.viewport_height - box.y1,
                    box.x1, box.y1, box.x2, box.y2,
                    GL_COLOR_BUFFER_BIT, GL_LINEAR));
            }

            /* This effectively makes damage the same as expanded_damage. */
            damage |= expanded_damage;
            GL_CALL(glBindTexture(GL_TEXTURE_2D, 0));
            OpenGL::render_end();
        };

        output->render->connect_signal("workspace-stream-pre",
            &workspace_stream_pre);

        /* workspace_stream_post is called after rendering each frame
         * when rendering a workspace. It gives us a chance to copy
         * the pixels back to the framebuffer that we saved in
         * workspace_stream_pre. */
        workspace_stream_post = [=] (wf::signal_data_t *data)
        {
            const auto& target_fb = static_cast<wf::stream_signal_t*>(data)->fb;
            OpenGL::render_begin(target_fb);
            /* Setup framebuffer I/O. target_fb contains the frame
             * rendered with expanded damage and artifacts on the edges.
             * saved_pixels has the the padded region of pixels to overwrite the
             * artifacts that blurring has left behind. */
            GL_CALL(glBindFramebuffer(GL_READ_FRAMEBUFFER, saved_pixels.fb));

            /* Copy pixels back from saved_pixels to target_fb. */
            for (const auto& box : padded_region)
            {
                GL_CALL(glBlitFramebuffer(box.x1, box.y1, box.x2, box.y2,
                    box.x1, target_fb.viewport_height - box.y2,
                    box.x2, target_fb.viewport_height - box.y1,
                    GL_COLOR_BUFFER_BIT, GL_LINEAR));
            }

            /* Reset stuff */
            padded_region.clear();
            GL_CALL(glBindTexture(GL_TEXTURE_2D, 0));
            OpenGL::render_end();
        };

        output->render->connect_signal("workspace-stream-post",
            &workspace_stream_post);
    }

    void fini() override
    {
        remove_transformers();

        output->rem_binding(&button_toggle);
        output->disconnect_signal("view-attached", &view_attached);
        output->disconnect_signal("view-mapped", &view_attached);
        output->disconnect_signal("view-detached", &view_detached);
        output->render->rem_effect(&frame_pre_paint);
        output->render->disconnect_signal("workspace-stream-pre",
            &workspace_stream_pre);
        output->render->disconnect_signal("workspace-stream-post",
            &workspace_stream_post);

        /* Call blur algorithm destructor */
        blur_algorithm = nullptr;

        OpenGL::render_begin();
        saved_pixels.release();
        OpenGL::render_end();
    }
};

DECLARE_WAYFIRE_PLUGIN(wayfire_blur);
