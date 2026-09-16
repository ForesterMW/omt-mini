// Tabbed settings. Changes are written to settings.ini as they are made;
// anything that needs a restart or a restart of a running mode says so.
#pragma once
#include "window.h"
#include "capture.h"

#include <vector>
#include <string>

class SettingsWindow : public Window {
public:
    void open_or_focus(int tab = 0);

protected:
    void on_render(ui::Ctx& ctx) override;
    bool on_close_request() override;
    const wchar_t* class_name() const override { return L"OMTMiniSettings"; }

private:
    void tab_general(ui::Ctx& ctx, const D2D1_RECT_F& area);
    void tab_sources(ui::Ctx& ctx, const D2D1_RECT_F& area);
    void tab_network(ui::Ctx& ctx, const D2D1_RECT_F& area);
    void tab_viewer(ui::Ctx& ctx, const D2D1_RECT_F& area);
    void tab_desktop(ui::Ctx& ctx, const D2D1_RECT_F& area);
    void tab_webcam(ui::Ctx& ctx, const D2D1_RECT_F& area);
    void tab_about(ui::Ctx& ctx, const D2D1_RECT_F& area);

    int  active_tab_ = 0;
    bool dirty_ = false;
    std::wstring discovery_text_;
    std::wstring port_start_text_, port_end_text_;
    std::wstring capture_name_text_;
    std::wstring status_message_;
    int64_t      status_until_ms_ = 0;
    std::vector<CaptureMonitor> monitors_;
    ui::ScrollState scroll_;

    // Sources tab
    std::wstring manual_address_text_;
    std::wstring manual_name_text_;
    std::wstring manual_error_;
};
