// The window the tray icon opens: everything available on the network, plus
// the state of the desktop sender and the webcam output.
#pragma once
#include "window.h"
#include "discovery.h"

#include <vector>
#include <string>

class SourcesWindow : public Window {
public:
    void open_or_focus();

protected:
    void on_render(ui::Ctx& ctx) override;
    bool on_close_request() override;
    const wchar_t* class_name() const override { return L"OMTMiniSources"; }

private:
    void draw_header(ui::Ctx& ctx);
    void draw_list(ui::Ctx& ctx, const D2D1_RECT_F& area);
    void draw_footer(ui::Ctx& ctx, const D2D1_RECT_F& area);

    ui::ScrollState scroll_;
    std::vector<DiscoveredSource> cached_;
    int hovered_row_ = -1;
};
