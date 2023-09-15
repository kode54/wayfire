#include "wayfire/core.hpp"
#include "wayfire/signal-definitions.hpp"
#include "wayfire/view.hpp"
#include <memory>
#include <wayfire/plugin.hpp>
#include <wayfire/view.hpp>
#include <wayfire/toplevel-view.hpp>
#include <wayfire/nonstd/wlroots-full.hpp>
#include <wayfire/window-manager.hpp>
#include "config.h"

class wayfire_xdg_activation_protocol_impl : public wf::plugin_interface_t
{
  public:
    void init() override
    {
        xdg_activation = wlr_xdg_activation_v1_create(wf::get_core().display);
        xdg_activation_request_activate.notify = xdg_activation_handle_request_activate;

        wl_signal_add(&xdg_activation->events.request_activate, &xdg_activation_request_activate);
    }

    void fini() override
    {}

    bool is_unloadable() override
    {
        return false;
    }

  private:
    static void xdg_activation_handle_request_activate(struct wl_listener *listener, void *data)
    {
        auto event = static_cast<const struct wlr_xdg_activation_v1_request_activate_event*>(data);

        wayfire_view view = wf::wl_surface_to_wayfire_view(event->surface->resource);
        if (!view)
        {
            LOGE("Could not get view");
            return;
        }

        auto toplevel = wf::toplevel_cast(view);
        if (!toplevel)
        {
            LOGE("Could not get toplevel view");
            return;
        }

        if (!event->token->seat)
        {
            LOGI("Denying focus request, seat wasn't supplied");
            return;
        }

        LOGI("Activating view");
        wf::get_core().default_wm->focus_request(toplevel);
    }

    struct wlr_xdg_activation_v1 *xdg_activation;
    struct wl_listener xdg_activation_request_activate;
};

DECLARE_WAYFIRE_PLUGIN(wayfire_xdg_activation_protocol_impl);
