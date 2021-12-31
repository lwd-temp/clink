// Copyright (c) 2021 Christopher Antos
// License: http://opensource.org/licenses/MIT

#include "pch.h"
#include <assert.h>
#include "textlist_impl.h"
#include "binder.h"
#include "editor_module.h"
#include "line_buffer.h"
#include "terminal_helpers.h"

#include <core/base.h>
#include <core/settings.h>
#include <core/str_compare.h>
#include <core/str_iter.h>
#include <rl/rl_commands.h>
#include <terminal/printer.h>
#include <terminal/ecma48_iter.h>

extern "C" {
#include <readline/readline.h>
#include <readline/rlprivate.h>
#include <readline/history.h>
extern int _rl_last_v_pos;
};



//------------------------------------------------------------------------------
enum {
    bind_id_textlist_up = 60,
    bind_id_textlist_down,
    bind_id_textlist_pgup,
    bind_id_textlist_pgdn,
    bind_id_textlist_home,
    bind_id_textlist_end,
    bind_id_textlist_findincr,
    bind_id_textlist_findnext,
    bind_id_textlist_findprev,
    bind_id_textlist_copy,
    bind_id_textlist_backspace,
    bind_id_textlist_escape,
    bind_id_textlist_enter,
    bind_id_textlist_insert,

    bind_id_textlist_catchall = binder::id_catchall_only_printable,
};

//------------------------------------------------------------------------------
extern setting_enum g_ignore_case;
extern setting_bool g_fuzzy_accent;
extern const char* get_popup_colors();
extern const char* get_popup_desc_colors();

//------------------------------------------------------------------------------
static textlist_impl* s_textlist = nullptr;
const int min_screen_cols = 20;

//------------------------------------------------------------------------------
static int make_item(const char* in, str_base& out)
{
    out.clear();

    int cells = 0;
    for (str_iter iter(in, strlen(in)); int c = iter.next(); in = iter.get_pointer())
    {
        if (unsigned(c) < ' ')
        {
            char ctrl = char(c) + '@';
            out.concat("^", 1);
            out.concat(&ctrl, 1);
            cells += 2;
        }
        else
        {
            out.concat(in, int(iter.get_pointer() - in));
            cells += clink_wcwidth(c);
        }
    }
    return cells;
}

//------------------------------------------------------------------------------
static int make_column(const char* in, const char* end, str_base& out)
{
    out.clear();

    int cells = 0;

    ecma48_state state;
    ecma48_iter iter(in, state, end ? int(end - in) : -1);
    while (const ecma48_code& code = iter.next())
        if (code.get_type() == ecma48_code::type_chars)
        {
            const char* p = code.get_pointer();
            for (str_iter inner_iter(code.get_pointer(), code.get_length());
                 int c = inner_iter.next();
                 p = inner_iter.get_pointer())
            {
                if (c == '\r' || c == '\n')
                {
                    out.concat(" ", 1);
                    cells++;
                }
                else if (unsigned(c) < ' ')
                {
                    char ctrl = char(c) + '@';
                    out.concat("^", 1);
                    out.concat(&ctrl, 1);
                    cells += 2;
                }
                else
                {
                    out.concat(p, int(inner_iter.get_pointer() - p));
                    cells += clink_wcwidth(c);
                }
            }
        }

    return cells;
}

//------------------------------------------------------------------------------
static void make_spaces(int num, str_base& out)
{
    out.clear();
    while (num > 0)
    {
        int chunk = min<int>(32, num);
        out.concat("                                ", chunk);
        num -= chunk;
    }
}

//------------------------------------------------------------------------------
static int limit_cells(const char* in, int limit, int& cells)
{
    cells = 0;
    str_iter iter(in, strlen(in));
    while (int c = iter.next())
    {
        cells += clink_wcwidth(c);
        if (cells >= limit)
            break;
    }
    return int(iter.get_pointer() - in);
}

//------------------------------------------------------------------------------
static bool strstr_compare(const str_base& needle, const char* haystack)
{
    if (haystack && *haystack)
    {
        str_iter sift(haystack);
        while (sift.more())
        {
            int cmp = str_compare(needle.c_str(), sift.get_pointer());
            if (cmp == -1 || cmp == needle.length())
                return true;
            sift.next();
        }
    }

    return false;
}



//------------------------------------------------------------------------------
textlist_impl::addl_columns::addl_columns(textlist_impl::item_store& store)
    : m_store(store)
{
}

//------------------------------------------------------------------------------
const char* textlist_impl::addl_columns::get_col_text(int row, int col) const
{
    return m_rows[row].column[col];
}

//------------------------------------------------------------------------------
int textlist_impl::addl_columns::get_col_width(int col) const
{
    return m_longest[col];
}

//------------------------------------------------------------------------------
const char* textlist_impl::addl_columns::add_entry(const char* ptr)
{
    size_t len_match = strlen(ptr);
    ptr += len_match + 1;

    const char* display = ptr;
    size_t len_display = strlen(ptr);
    ptr += len_display + 1;

    column_text column_text = {};
    if (*ptr)
    {
        str<> tmp;
        int col = 0;
        while (col < sizeof_array(column_text.column))
        {
            const char* tab = strchr(ptr, '\t');
            const int cells = make_column(ptr, tab, tmp);
            column_text.column[col] = m_store.add(tmp.c_str());
            m_longest[col] = max<int>(m_longest[col], cells);
            ptr = tab;
            if (!ptr)
                break;
            col++;
            ptr++;
        }
    }

    m_rows.emplace_back(std::move(column_text));

    return display;
}

//------------------------------------------------------------------------------
void textlist_impl::addl_columns::clear()
{
    std::vector<column_text> zap;
    m_rows = std::move(zap);
    memset(&m_longest, 0, sizeof(m_longest));
}



//------------------------------------------------------------------------------
textlist_impl::textlist_impl(input_dispatcher& dispatcher)
    : m_dispatcher(dispatcher)
    , m_columns(m_store)
{
}

//------------------------------------------------------------------------------
popup_results textlist_impl::activate(const char* title, const char** entries, int count, int index, bool reverse, int history_mode, const entry_info* infos, bool has_columns)
{
    reset();
    m_results.clear();

    assert(m_buffer);
    if (!m_buffer)
        return popup_result::error;

    if (!entries || count <= 0)
        return popup_result::error;

    // Doesn't make sense to record macro with a popup list.
    if (RL_ISSTATE(RL_STATE_MACRODEF) != 0)
        return popup_result::error;

    // Make sure there's room.
    m_reverse = reverse;
    m_history_mode = (history_mode != 0);
    m_win_history = (history_mode == 2);
    update_layout();
    if (m_visible_rows <= 0)
    {
        m_reverse = false;
        m_history_mode = false;
        m_win_history = false;
        return popup_result::error;
    }

    // Gather the items.
    str<> tmp;
    m_entries = entries;
    m_infos = infos;
    m_count = count;
    for (int i = 0; i < count; i++)
    {
        const char* text;
        if (has_columns)
            text = m_columns.add_entry(m_entries[i]);
        else
            text = m_entries[i];
        m_longest = max<int>(m_longest, make_item(text, tmp));
        m_items.push_back(m_store.add(tmp.c_str()));
    }
    m_has_columns = has_columns;

    if (title && *title)
        m_default_title = title;

    // Initialize the view.
    if (index < 0)
    {
        m_index = m_count - 1;
        m_top = max<int>(0, m_count - m_visible_rows);
    }
    else
    {
        m_index = index;
        m_top = max<int>(0, min<int>(m_index - (m_visible_rows / 2), m_count - m_visible_rows));
    }

    show_cursor(false);
    lock_cursor(true);

    assert(!m_active);
    m_active = true;
    update_display();

    m_dispatcher.dispatch(m_bind_group);

    // Cancel if the dispatch loop is left unexpectedly (e.g. certain errors).
    if (m_active)
        cancel(popup_result::cancel);

    assert(!m_active);
    update_display();

    _rl_refresh_line();
    rl_display_fixed = 1;

    lock_cursor(false);
    show_cursor(true);

    popup_results results;
    results.m_result = m_results.m_result;
    results.m_index = m_results.m_index;
    results.m_text = std::move(m_results.m_text);

    reset();
    m_results.clear();

    return results;
}

//------------------------------------------------------------------------------
void textlist_impl::bind_input(binder& binder)
{
    const char* esc = get_bindable_esc();

    m_bind_group = binder.create_group("textlist");
    binder.bind(m_bind_group, "\\e[A", bind_id_textlist_up);            // Up
    binder.bind(m_bind_group, "\\e[B", bind_id_textlist_down);          // Down
    binder.bind(m_bind_group, "\\e[5~", bind_id_textlist_pgup);         // PgUp
    binder.bind(m_bind_group, "\\e[6~", bind_id_textlist_pgdn);         // PgDn
    binder.bind(m_bind_group, "\\e[H", bind_id_textlist_home);          // Home
    binder.bind(m_bind_group, "\\e[F", bind_id_textlist_end);           // End
    binder.bind(m_bind_group, "\\eOR", bind_id_textlist_findnext);      // F3
    binder.bind(m_bind_group, "\\e[1;2R", bind_id_textlist_findprev);   // Shift+F3
    binder.bind(m_bind_group, "^l", bind_id_textlist_findnext);         // Ctrl+L
    binder.bind(m_bind_group, "\\e[27;6;76~", bind_id_textlist_findprev); // Ctrl+Shift+L
    binder.bind(m_bind_group, "^c", bind_id_textlist_copy);             // Ctrl+C
    binder.bind(m_bind_group, "^h", bind_id_textlist_backspace);        // Backspace
    binder.bind(m_bind_group, "\\r", bind_id_textlist_enter);           // Enter
    binder.bind(m_bind_group, "\\e[27;2;13~", bind_id_textlist_insert); // Shift+Enter
    binder.bind(m_bind_group, "\\e[27;5;13~", bind_id_textlist_insert); // Ctrl+Enter

    binder.bind(m_bind_group, "^g", bind_id_textlist_escape);           // Ctrl+G
    if (esc)
        binder.bind(m_bind_group, esc, bind_id_textlist_escape);        // Esc

    binder.bind(m_bind_group, "", bind_id_textlist_catchall);
}

//------------------------------------------------------------------------------
void textlist_impl::on_begin_line(const context& context)
{
    assert(!s_textlist);
    s_textlist = this;
    m_buffer = &context.buffer;
    m_printer = &context.printer;

    m_screen_cols = context.printer.get_columns();
    m_screen_rows = context.printer.get_rows();
    update_layout();
}

//------------------------------------------------------------------------------
void textlist_impl::on_end_line()
{
    s_textlist = nullptr;
    m_buffer = nullptr;
    m_printer = nullptr;
}

//------------------------------------------------------------------------------
static void advance_index(int& i, int direction, int max_count)
{
    i += direction;
    if (direction < 0)
    {
        if (i < 0)
            i = max_count - 1;
    }
    else
    {
        if (i >= max_count)
            i = 0;
    }
}

//------------------------------------------------------------------------------
void textlist_impl::on_input(const input& _input, result& result, const context& context)
{
    assert(m_active);

    input input = _input;
    bool set_input_clears_needle = true;
    bool from_begin = false;
    bool need_display = false;

    // Cancel if no room.
    if (m_visible_rows <= 0)
    {
        cancel(popup_result::cancel);
        return;
    }

    switch (input.id)
    {
    case bind_id_textlist_up:
        m_index--;
        if (m_index < 0)
            m_index = _rl_menu_complete_wraparound ? m_count - 1 : 0;
navigated:
        update_display();
        break;
    case bind_id_textlist_down:
        m_index++;
        if (m_index >= m_count)
            m_index = _rl_menu_complete_wraparound ? 0 : m_count - 1;
        goto navigated;

    case bind_id_textlist_home:
        m_index = 0;
        goto navigated;
    case bind_id_textlist_end:
        m_index = m_count - 1;
        goto navigated;

    case bind_id_textlist_pgup:
    case bind_id_textlist_pgdn:
        {
            const int y = m_index;
            const int rows = min<int>(m_count, m_visible_rows);

            // Use rows as the page size (vs the more common rows-1) for
            // compatibility with Conhost's F7 popup list behavior.
            if (input.id == bind_id_textlist_pgup)
            {
                if (y > 0)
                {
                    int new_y = max<int>(0, (y == m_top) ? y - rows : m_top);
                    m_index += (new_y - y);
                    goto navigated;
                }
            }
            else if (input.id == bind_id_textlist_pgdn)
            {
                if (y < m_count - 1)
                {
                    int bottom_y = m_top + rows - 1;
                    int new_y = min<int>(m_count - 1, (y == bottom_y) ? y + rows : bottom_y);
                    m_index += (new_y - y);
                    if (m_index > m_count - 1)
                    {
                        set_top(max<int>(0, m_count - m_visible_rows));
                        m_index = m_count - 1;
                    }
                    goto navigated;
                }
            }
        }
        break;

    case bind_id_textlist_findnext:
    case bind_id_textlist_findprev:
        set_input_clears_needle = false;
        if (m_win_history)
            break;
find:
        if (m_win_history)
        {
            lock_cursor(false);
            show_cursor(true);
            rl_ding();
            show_cursor(false);
            lock_cursor(true);
        }
        else
        {
            int direction = (input.id == bind_id_textlist_findprev) ? -1 : 1;
            if (m_reverse)
                direction = 0 - direction;

            int mode = g_ignore_case.get();
            if (mode < 0 || mode >= str_compare_scope::num_scope_values)
                mode = str_compare_scope::exact;
            str_compare_scope _(mode, g_fuzzy_accent.get());

            int i = m_index;
            if (from_begin)
                i = m_reverse ? m_count - 1 : 0;

            if (input.id == bind_id_textlist_findnext || input.id == bind_id_textlist_findprev)
                advance_index(i, direction, m_count);

            int original = i;
            while (true)
            {
                bool match = strstr_compare(m_needle, m_items[i]);
                if (m_has_columns)
                {
                    for (int col = 0; !match && col < max_columns; col++)
                        match = strstr_compare(m_needle, m_columns.get_col_text(i, col));
                }

                if (match)
                {
                    m_index = i;
                    if (m_index < m_top || m_index >= m_top + m_visible_rows)
                        m_top = max<int>(0, min<int>(m_index, m_count - m_visible_rows));
                    m_prev_displayed = -1;
                    need_display = true;
                    break;
                }

                advance_index(i, direction, m_count);
                if (i == m_index)
                    break;
            }

            if (need_display)
                update_display();
        }
        break;

    case bind_id_textlist_copy:
        {
            const char* text = m_entries[m_index];
            os::set_clipboard_text(text, int(strlen(text)));
            set_input_clears_needle = false;
        }
        break;

    case bind_id_textlist_escape:
        cancel(popup_result::cancel);
        return;

    case bind_id_textlist_enter:
        cancel(popup_result::use);
        return;

    case bind_id_textlist_insert:
        cancel(popup_result::select);
        return;

    case bind_id_textlist_backspace:
    case bind_id_textlist_catchall:
        {
            bool refresh = false;

            set_input_clears_needle = false;

            if (input.id == bind_id_textlist_backspace)
            {
                if (!m_needle.length())
                    break;
                int point = _rl_find_prev_mbchar(const_cast<char*>(m_needle.c_str()), m_needle.length(), MB_FIND_NONZERO);
                m_needle.truncate(point);
                need_display = true;
                from_begin = !m_win_history;
                refresh = true;
                goto update_needle;
            }

            // Collect the input.
            {
                if (m_input_clears_needle)
                {
                    assert(!m_win_history);
                    m_needle.clear();
                    m_needle_is_number = false;
                    m_input_clears_needle = false;
                }

                str_iter iter(input.keys, input.len);
                const char* seq = iter.get_pointer();
                while (iter.more())
                {
                    unsigned int c = iter.next();
                    if (!m_win_history)
                    {
                        refresh = m_has_override_title;
                        m_override_title.clear();
                        m_needle.concat(seq, int(iter.get_pointer() - seq));
                        need_display = true;
                    }
                    else if (c >= '0' && c <= '9')
                    {
                        if (!m_needle_is_number)
                        {
                            refresh = m_has_override_title;
                            m_override_title.clear();
                            m_needle.clear();
                            m_needle_is_number = true;
                        }
                        if (m_needle.length() < 6)
                        {
                            char digit = char(c);
                            m_needle.concat(&digit, 1);
                        }
                    }
                    else
                    {
                        refresh = m_has_override_title;
                        m_override_title.clear();
                        m_needle.clear();
                        m_needle.concat(seq, int(iter.get_pointer() - seq));
                        m_needle_is_number = false;
                    }
                    seq = iter.get_pointer();
                }
            }

            // Handle the input.
update_needle:
            if (!m_win_history)
            {
                input.id = bind_id_textlist_findincr;
                m_override_title.clear();
                if (m_needle.length())
                    m_override_title.format("find: %-10s", m_needle.c_str());
                goto find;
            }
            else if (m_needle_is_number)
            {
                if (m_needle.length())
                {
                    refresh = true;
                    m_override_title.clear();
                    m_override_title.format("enter history number: %-6s", m_needle.c_str());
                    int i = atoi(m_needle.c_str());
                    if (m_infos)
                    {
                        int lookup = 0;
                        char lookupstr[16];
                        char needlestr[16];
                        _itoa_s(i, needlestr, 10);
                        const int needlestr_len = int(strlen(needlestr));
                        while (lookup < m_count)
                        {
                            _itoa_s(m_infos[lookup].index + 1, lookupstr, 10);
                            if (strncmp(needlestr, lookupstr, needlestr_len) == 0)
                            {
                                i = lookup;
                                break;
                            }
                            lookup++;
                        }
                        // If the input history history number isn't found, i is
                        // m_count and correctly skips updating m_index.
                        i = lookup;
                    }
                    else
                    {
                        i--;
                    }
                    if (i >= 0 && i < m_count)
                    {
                        m_index = i;
                        if (m_index < m_top || m_index >= m_top + m_visible_rows)
                            m_top = max<int>(0, min<int>(m_index - (m_visible_rows / 2), m_count - m_visible_rows));
                        m_prev_displayed = -1;
                        refresh = true;
                    }
                }
                else if (m_override_title.length())
                {
                    refresh = true;
                    m_override_title.clear();
                }
            }
            else if (m_needle.length())
            {
                str_compare_scope _(str_compare_scope::caseless, true/*fuzzy_accent*/);

                int i = m_index;
                while (true)
                {
                    i--;
                    if (i < 0)
                        i = m_count - 1;
                    if (i == m_index)
                        break;

                    int cmp = str_compare(m_needle.c_str(), m_items[i]);
                    if (cmp == -1 || cmp == m_needle.length())
                    {
                        m_index = i;
                        if (m_index < m_top || m_index >= m_top + m_visible_rows)
                            m_top = max<int>(0, min<int>(m_index, m_count - m_visible_rows));
                        m_prev_displayed = -1;
                        refresh = true;
                        break;
                    }
                }
            }

            if (refresh)
                update_display();
        }
        break;
    }

    if (set_input_clears_needle && !m_win_history)
        m_input_clears_needle = true;

    // Keep dispatching input.
    result.loop();
}

//------------------------------------------------------------------------------
void textlist_impl::on_matches_changed(const context& context, const line_state& line, const char* needle)
{
}

//------------------------------------------------------------------------------
void textlist_impl::on_terminal_resize(int columns, int rows, const context& context)
{
    m_screen_cols = columns;
    m_screen_rows = rows;

    if (m_active)
        cancel(popup_result::cancel);
}

//------------------------------------------------------------------------------
void textlist_impl::cancel(popup_result result)
{
    assert(m_active);

    m_results.clear();
    m_results.m_result = result;
    if (result == popup_result::use || result == popup_result::select)
    {
        if (m_index >= 0 && m_index < m_count)
        {
            m_results.m_index = m_index;
            m_results.m_text = m_entries[m_index];
        }
    }

    m_active = false;
}

//------------------------------------------------------------------------------
void textlist_impl::update_layout()
{
    int slop_rows = 2;
    int border_rows = 2;
    int target_rows = m_history_mode ? 20 : 10;

    m_visible_rows = min<int>(target_rows, (m_screen_rows / 2) - border_rows - slop_rows);

    if (m_screen_cols <= min_screen_cols)
        m_visible_rows = 0;
}

//------------------------------------------------------------------------------
void textlist_impl::update_top()
{
    const int y = m_index;
    if (m_top > y)
    {
        set_top(y);
    }
    else
    {
        const int rows = min<int>(m_count, m_visible_rows);
        int top = max<int>(0, y - (rows - 1));
        if (m_top < top)
            set_top(top);
    }
    assert(m_top >= 0);
    assert(m_top <= max<int>(0, m_count - m_visible_rows));
}

//------------------------------------------------------------------------------
void textlist_impl::update_display()
{
    if (m_visible_rows > 0)
    {
        // Remember the cursor position so it can be restored later to stay
        // consistent with Readline's view of the world.
        CONSOLE_SCREEN_BUFFER_INFO csbi;
        const HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
        GetConsoleScreenBufferInfo(h, &csbi);
        COORD restore = csbi.dwCursorPosition;
        const int vpos = _rl_last_v_pos;
        const int cpos = _rl_last_c_pos;

        // Move cursor to next line.  I.e. the list goes immediately below the
        // cursor line and may overlay some lines of input.
        m_printer->print("\n");

        // Display list.
        int up = 1;
        bool move_to_end = true;
        const int count = m_count;
        if (m_active && count > 0)
        {
            update_top();

            const bool draw_border = (m_prev_displayed < 0) || m_override_title.length() || m_has_override_title;
            m_has_override_title = !m_override_title.empty();

            str<> tmp;
            int max_num_len = 0;
            if (m_history_mode)
            {
                tmp.format("%u", m_infos ? m_infos[m_count - 1].index + 1 : m_count);
                max_num_len = tmp.length();
            }

            int longest = m_longest + (max_num_len ? max_num_len + 2 : 0); // +2 for ": ".
            if (m_has_columns)
            {
                for (int i = 0; i < max_columns; i++)
                {
                    const int x = m_columns.get_col_width(i);
                    if (x)
                        longest += 2 + x;
                }
            }
            longest = max<int>(longest, 40);

            const int effective_screen_cols = (m_screen_cols < 40) ? m_screen_cols : max<int>(40, m_screen_cols - 4);
            const int col_width = min<int>(longest + 2, effective_screen_cols); // +2 for borders.

            str<> noescape;
            str<> left;
            str<> horzline;
            for (int i = col_width - 2; i--;)
                horzline.concat("\xe2\x94\x80", 3);

            {
                int x = csbi.dwCursorPosition.X - ((col_width + 1) / 2);
                int center_x = (m_screen_cols - effective_screen_cols) / 2;
                if (x + col_width > center_x + effective_screen_cols)
                    x = m_screen_cols - center_x - col_width;
                if (x < center_x)
                    x = center_x;
                if (x > 0)
                    left.format("\x1b[%uG", x + 1);
            }

            str<32> color;
            color.format("\x1b[%sm", get_popup_colors());

            str<32> desc_color;
            desc_color.format("\x1b[%sm", get_popup_desc_colors());

            str<32> modmark;
            modmark.format("%s*%s", desc_color.c_str(), color.c_str());

            // Display border.
            if (draw_border)
            {
                const str_base* topline = &horzline;
                if (m_default_title.length() || m_override_title.length())
                {
                    const char* title = m_has_override_title ? m_override_title.c_str() : m_default_title.c_str();
                    int title_cells = 0;
                    int title_len = 0;

                    {
                        const char* walk = title;
                        int remaining = col_width - (2 + 2 + 2); // Corners, bars, spaces.
                        str_iter iter(title);
                        while (int c = iter.next())
                        {
                            const int width = clink_wcwidth(c);
                            if (width > remaining)
                                break;
                            title_cells += width;
                            remaining -= width;
                            title_len = iter.get_pointer() - title;
                        }
                    }

                    int x = (col_width - 2 - title_cells) / 2;
                    tmp.clear();
                    x--;

                    for (int i = x; i-- > 0;)
                    {
                        if (i == 0 && m_has_override_title)
                            tmp.concat("\xe2\x94\xa4", 3);
                        else
                            tmp.concat("\xe2\x94\x80", 3);
                    }

                    x += 1 + title_cells + 1;
                    tmp.concat(" ", 1);
                    tmp.concat(title, title_len);
                    tmp.concat(" ", 1);

                    bool cap = m_has_override_title;
                    for (int i = col_width - 2 - x; i-- > 0;)
                    {
                        if (cap)
                        {
                            cap = false;
                            tmp.concat("\xe2\x94\x9c", 3);
                        }
                        else
                        {
                            tmp.concat("\xe2\x94\x80", 3);
                        }
                    }

                    topline = &tmp;
                }

                m_printer->print(left.c_str(), left.length());
                m_printer->print(color.c_str(), color.length());
                m_printer->print("\xe2\x94\x8c");                       // ┌
                m_printer->print(topline->c_str(), topline->length());  // ─
                m_printer->print("\xe2\x94\x90\x1b[m");                 // ┐
            }

            // Display items.
            for (int row = 0; row < m_visible_rows; row++)
            {
                const int i = m_top + row;
                if (i >= count)
                    break;

                rl_crlf();
                up++;

                move_to_end = true;
                if (m_prev_displayed < 0 ||
                    i == m_index ||
                    i == m_prev_displayed)
                {
                    m_printer->print(left.c_str(), left.length());
                    m_printer->print(color.c_str(), color.length());
                    m_printer->print("\xe2\x94\x82");               // │

                    if (i == m_index)
                        m_printer->print("\x1b[7m");

                    int spaces = col_width - 2;

                    if (m_history_mode)
                    {
                        const int history_index = m_infos ? m_infos[i].index : i;
                        const char* mark = (!m_infos || !m_infos[i].marked ? " " :
                                            i == m_index ? "*" :
                                            modmark.c_str());
                        tmp.clear();
                        tmp.format("%*u:%s", max_num_len, history_index + 1, mark);
                        m_printer->print(tmp.c_str(), tmp.length());// history number
                        spaces -= tmp.length();
                        if (mark == modmark.c_str())
                            spaces += modmark.length() - 1;
                    }

                    int cell_len;
                    const int char_len = limit_cells(m_items[i], spaces, cell_len);
                    m_printer->print(m_items[i], char_len);         // main text
                    spaces -= cell_len;

                    if (m_has_columns)
                    {
                        if (i != m_index)
                            m_printer->print(desc_color.c_str(), desc_color.length());

                        for (int col = 0; col < max_columns && spaces > 0; col++)
                        {
                            tmp.clear();
                            tmp.concat("  ", 2);
                            tmp.concat(m_columns.get_col_text(i, col));
                            const int col_len = limit_cells(tmp.c_str(), spaces, cell_len);
                            m_printer->print(tmp.c_str(), col_len); // column text
                            spaces -= cell_len;

                            int pad = min<int>(spaces, m_columns.get_col_width(col) - (cell_len - 2));
                            if (pad > 0)
                            {
                                make_spaces(pad, tmp);
                                m_printer->print(tmp.c_str(), tmp.length()); // spaces
                                spaces -= tmp.length();
                            }
                        }
                    }

                    make_spaces(spaces, tmp);
                    m_printer->print(tmp.c_str(), tmp.length());    // spaces

                    if (i == m_index)
                        m_printer->print("\x1b[27m");

                    if (m_has_columns)
                        m_printer->print(color.c_str(), color.length());

                    m_printer->print("\xe2\x94\x82\x1b[m");         // │
                }
            }

            // Display border.
            if (draw_border)
            {
                rl_crlf();
                up++;
                m_printer->print(left.c_str(), left.length());
                m_printer->print(color.c_str(), color.length());
                m_printer->print("\xe2\x94\x94");                       // └
                m_printer->print(horzline.c_str(), horzline.length());  // ─
                m_printer->print("\xe2\x94\x98\x1b[m");                 // ┘
            }

            m_prev_displayed = m_index;
        }
        else
        {
            // Clear to end of screen.
            m_printer->print("\x1b[m\x1b[J");

            m_prev_displayed = -1;
        }

        // Restore cursor position.
        if (up > 0)
        {
            str<16> s;
            s.format("\x1b[%dA", up);
            m_printer->print(s.c_str(), s.length());
        }
        _rl_move_vert(vpos);
        _rl_last_c_pos = cpos;
        GetConsoleScreenBufferInfo(h, &csbi);
        restore.Y = csbi.dwCursorPosition.Y;
        SetConsoleCursorPosition(h, restore);
    }
}

//------------------------------------------------------------------------------
void textlist_impl::set_top(int top)
{
    assert(top >= 0);
    assert(top <= max<int>(0, m_count - m_visible_rows));
    if (top != m_top)
    {
        m_top = top;
        m_prev_displayed = -1;
    }
}

//------------------------------------------------------------------------------
void textlist_impl::reset()
{
    std::vector<const char*> zap_items;

    // Don't reset screen row and cols; they stay in sync with the terminal.

    m_visible_rows = 0;
    m_default_title.clear();
    m_override_title.clear();
    m_has_override_title = false;

    m_count = 0;
    m_entries = nullptr;    // Don't free; is only borrowed.
    m_infos = nullptr;      // Don't free; is only borrowed.
    m_items = std::move(zap_items);
    m_longest = 0;
    m_columns.clear();
    m_history_mode = false;
    m_win_history = false;
    m_has_columns = false;

    m_top = 0;
    m_index = 0;
    m_prev_displayed = -1;

    m_needle.clear();
    m_needle_is_number = false;
    m_input_clears_needle = false;

    m_store.clear();
}



//------------------------------------------------------------------------------
textlist_impl::item_store::~item_store()
{
    clear();
}

//------------------------------------------------------------------------------
const char* textlist_impl::item_store::add(const char* item)
{
    unsigned len = unsigned(strlen(item) + 1);

    if (len > m_back - m_front)
    {
        page* p = (page*)VirtualAlloc(nullptr, pagesize, MEM_COMMIT, PAGE_READWRITE);
        p->next = m_page;
        m_page = p;
        m_front = &p->data - (const char*)p;
        m_back = 65536;
    }

    char* p = reinterpret_cast<char*>(m_page) + m_front;
    memcpy(p, item, len);
    m_front += len;
    return p;
}

//------------------------------------------------------------------------------
void textlist_impl::item_store::clear()
{
    while (m_page)
    {
        page* p = m_page;
        m_page = p->next;
        VirtualFree(p, 0, MEM_RELEASE);
    }

    m_front = m_back = 0;
}



//------------------------------------------------------------------------------
popup_results activate_text_list(const char* title, const char** entries, int count, int current, bool has_columns)
{
    if (!s_textlist)
        return popup_result::error;

    return s_textlist->activate(title, entries, count, current, false/*reverse*/, false/*history_mode*/, nullptr, has_columns);
}

//------------------------------------------------------------------------------
popup_results activate_directories_text_list(const char** dirs, int count)
{
    if (!s_textlist)
        return popup_result::error;

    return s_textlist->activate("Directories", dirs, count, count - 1, true/*reverse*/, false/*history_mode*/, nullptr, false);
}

//------------------------------------------------------------------------------
popup_results activate_history_text_list(const char** history, int count, int current, const entry_info* infos, int history_mode)
{
    if (!s_textlist)
        return popup_result::error;

    assert(current >= 0);
    assert(current < count);
    return s_textlist->activate("History", history, count, current, true/*reverse*/, history_mode, infos, false);
}