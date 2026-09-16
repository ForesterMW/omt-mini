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
    bool on_message(UINT msg, WPARAM wp, LPARAM lp, LRESULT& result) override;
    const wchar_t* class_name() const override { return L"OMTMiniSources"; }

private:
    void draw_header(ui::Ctx& ctx);
    float draw_add_panel(ui::Ctx& ctx, float top);
    void commit_add();
    void finish_scan();
    void draw_list(ui::Ctx& ctx, const D2D1_RECT_F& area);
    void draw_footer(ui::Ctx& ctx, const D2D1_RECT_F& area);

    ui::ScrollState scroll_;
    std::vector<DiscoveredSource> cached_;
    std::string  selected_address_;
    bool         adding_ = false;
    std::wstring add_address_text_;
    std::wstring add_name_text_;
    std::wstring add_error_;
    std::wstring toast_;
    int64_t      toast_until_ms_ = 0;
    bool         scan_pending_ = false;   // a finished scan waiting to be taken
};
