// Immediate mode widget layer drawn with Direct2D.
//
// Windows redraw on demand rather than on a timer: a widget that is mid
// animation sets needs_redraw(), and the window loop schedules exactly one
// more frame. An idle settings window therefore costs nothing.
#pragma once
#include "gfx.h"

#include <string>
#include <vector>
#include <map>
#include <functional>

namespace ui {

using gfx::Font;

// Widget identity. Callers pass a stable integer, usually a line number or an
// enum value; container widgets mix in an index.
using Id = uint32_t;
constexpr Id kNoId = 0;

struct Input {
    float mouse_x = -1.0f, mouse_y = -1.0f;
    bool  mouse_down = false;       // button currently held
    bool  mouse_pressed = false;    // pressed during this frame
    bool  mouse_released = false;
    bool  mouse_double = false;
    bool  right_pressed = false;
    float wheel = 0.0f;             // notches, positive is away from the user

    std::wstring typed;             // characters entered this frame
    int  key = 0;                   // virtual key code pressed this frame
    bool shift = false, ctrl = false;

    void clear_events() {
        mouse_pressed = mouse_released = mouse_double = right_pressed = false;
        wheel = 0.0f;
        typed.clear();
        key = 0;
    }
};

enum class Align { Left, Center, Right };

enum class ButtonStyle { Normal, Primary, Danger, Ghost, Toolbar };

struct ScrollState {
    float offset = 0.0f;
    float content_height = 0.0f;
    float view_height = 0.0f;
};

class Ctx {
public:
    // ---- frame ----
    void begin(ID2D1DeviceContext* dc, float width, float height, Input* input);
    void end();

    bool  needs_redraw() const { return needs_redraw_; }
    void  request_redraw() { needs_redraw_ = true; }
    float width()  const { return width_; }
    float height() const { return height_; }
    Input& input() { return *input_; }

    // True while a popup (dropdown list) is capturing input.
    bool popup_open() const { return open_dropdown_ != kNoId; }
    void close_popup() { open_dropdown_ = kNoId; }

    // ---- primitives ----
    void fill_rect(const D2D1_RECT_F& r, const D2D1_COLOR_F& c, float radius = 0.0f);
    void stroke_rect(const D2D1_RECT_F& r, const D2D1_COLOR_F& c,
                     float thickness = 1.0f, float radius = 0.0f);
    void line(float x0, float y0, float x1, float y1, const D2D1_COLOR_F& c, float thickness = 1.0f);
    void text(const D2D1_RECT_F& r, const std::wstring& s, Font f,
              const D2D1_COLOR_F& c, Align a = Align::Left, bool vcenter = true);
    void text_wrapped(const D2D1_RECT_F& r, const std::wstring& s, Font f, const D2D1_COLOR_F& c);
    float text_width(const std::wstring& s, Font f);
    void push_clip(const D2D1_RECT_F& r);
    void pop_clip();

    // Draws one of the small built in glyphs (chevrons, close, gear, plus).
    enum class Glyph { ChevronDown, ChevronRight, Close, Check, Dot, Plus, Minus, Gear, Pop };
    void glyph(Glyph g, const D2D1_RECT_F& r, const D2D1_COLOR_F& c, float thickness = 1.5f);

    // ---- hit testing ----
    bool hovered(const D2D1_RECT_F& r) const;

    // ---- widgets ----
    bool button(Id id, const D2D1_RECT_F& r, const std::wstring& label,
                ButtonStyle style = ButtonStyle::Normal, bool enabled = true);
    bool icon_button(Id id, const D2D1_RECT_F& r, Glyph g, const std::wstring& tooltip = L"");
    // A read only indicator lamp. Not clickable by design: this is a monitor
    // reporting what a source says about itself, not a control.
    void lamp(const D2D1_RECT_F& r, const std::wstring& label, bool lit,
              const D2D1_COLOR_F& colour);
    bool toggle(Id id, const D2D1_RECT_F& r, bool* value, const std::wstring& label);
    bool checkbox(Id id, const D2D1_RECT_F& r, bool* value, const std::wstring& label);
    bool slider(Id id, const D2D1_RECT_F& r, float* value, float min_v, float max_v);
    bool text_field(Id id, const D2D1_RECT_F& r, std::wstring* value,
                    const std::wstring& placeholder = L"", bool numeric = false);
    // Returns true when the selection changed.
    bool dropdown(Id id, const D2D1_RECT_F& r, const std::vector<std::wstring>& items, int* index);
    // Horizontal tab strip. Returns true when the active tab changed.
    //
    // Consumes ids id+1 through id+labels.size() for the individual tabs, so
    // callers must leave that range free.
    bool tabs(Id id, const D2D1_RECT_F& r, const std::vector<std::wstring>& labels, int* active);
    // A small tinted label, for example "this machine" or "direct". Returns
    // the width it occupied so the caller can lay out what follows it.
    float badge(float x, float y, float height, const std::wstring& label,
                const D2D1_COLOR_F& colour);
    // A small filled dot, for online and offline indication.
    void status_dot(float cx, float cy, float radius, const D2D1_COLOR_F& colour);
    void section_label(const D2D1_RECT_F& r, const std::wstring& label);
    void separator(float x0, float x1, float y);

    // ---- scrolling ----
    void begin_scroll(Id id, const D2D1_RECT_F& r, ScrollState* state);
    void end_scroll();

    // Animation helper: eases a per-widget value towards target, and keeps the
    // window redrawing until it settles.
    float animate(Id id, float target, float speed = 14.0f);

private:
    D2D1_COLOR_F mix(const D2D1_COLOR_F& a, const D2D1_COLOR_F& b, float t) const;
    ID2D1SolidColorBrush* brush(const D2D1_COLOR_F& c);
    bool  consume_click(Id id, const D2D1_RECT_F& r, bool enabled = true);

    ID2D1DeviceContext* dc_ = nullptr;
    Input*  input_ = nullptr;
    float   width_ = 0.0f, height_ = 0.0f;
    bool    needs_redraw_ = false;
    double  last_time_ = 0.0;
    float   dt_ = 0.016f;

    gfx::ComPtr<ID2D1SolidColorBrush> brush_;
    std::vector<D2D1_RECT_F> clip_stack_;

    Id hot_ = kNoId;
    Id active_ = kNoId;
    Id focus_ = kNoId;
    Id open_dropdown_ = kNoId;

    // Deferred popup drawing so dropdown lists paint above everything else.
    //
    // The selection is carried as a value, never as a pointer into the caller.
    // The popup is drawn from end(), by which time the caller's local index
    // variable has already gone out of scope, so writing through a pointer to
    // it would be writing to dead stack.
    struct PendingPopup {
        D2D1_RECT_F anchor;
        std::vector<std::wstring> items;
        int         selected = -1;
        Id          id = kNoId;
    };
    PendingPopup popup_;
    bool popup_pending_ = false;
    // Set when end() commits a selection; applied by dropdown() on the next
    // frame, which is why these deliberately survive begin().
    Id   dropdown_changed_ = kNoId;
    int  dropdown_value_ = -1;

    // Every id claimed this frame. Two widgets sharing one id fight over the
    // active state: whichever draws first eats the release and the other never
    // sees its click, which shows up as a control that works only sometimes.
    void claim(Id id);
    std::vector<Id> claimed_;

    std::map<Id, float> anim_;
    std::map<Id, size_t> caret_;
    ScrollState* scroll_ = nullptr;
    D2D1_RECT_F  scroll_rect_{};
    Id           scroll_id_ = kNoId;
    float        saved_mouse_y_ = 0.0f;
    bool         scroll_active_ = false;
};

// Layout helpers, kept free standing so windows can use them without a Ctx.
D2D1_RECT_F rect(float x, float y, float w, float h);
D2D1_RECT_F inset(const D2D1_RECT_F& r, float dx, float dy);
D2D1_RECT_F row(const D2D1_RECT_F& area, float y, float height);
bool contains(const D2D1_RECT_F& r, float x, float y);

// Standard metrics, so every window lines up.
namespace metric {
constexpr float kTitleBar   = 38.0f;
constexpr float kRowHeight  = 34.0f;
constexpr float kPad        = 14.0f;
constexpr float kRadius     = 6.0f;
constexpr float kFieldWidth = 220.0f;
}

} // namespace ui
