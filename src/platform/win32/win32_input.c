// =============================================================================
//  src/platform/win32/win32_input.c —— 输入状态机与按键映射
//
//  本文件有两部分：
//   1. 与平台无关的输入状态机（双缓冲、边沿检测、事件注入）
//   2. Win32 虚拟键码 -> key_t 的映射表
//
//  第 1 部分刻意写成不依赖任何 Win32 类型，这样单元测试可以脱离窗口
//  构造任意输入序列来验证边沿检测逻辑。
// =============================================================================
#include "engine/platform/input.h"

#include <string.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "engine/core/assert.h"

// =============================================================================
//  状态机
// =============================================================================
void input_begin_frame(input_state_t *state)
{
    ASSERT(state != NULL);

    // previous <- current。这一句是边沿检测的全部秘密：
    // 复制之后 current 才开始接收本帧的事件。
    memcpy(state->keys_previous, state->keys, sizeof(state->keys));
    memcpy(state->mouse_buttons_previous, state->mouse_buttons, sizeof(state->mouse_buttons));

    // 本帧增量清零。注意 mouse_x/mouse_y 不清 —— 那是绝对位置，
    // 跨帧保持；dx/dy 才是增量。
    state->mouse_dx = 0.0f;
    state->mouse_dy = 0.0f;
    state->scroll_dx = 0.0f;
    state->scroll_dy = 0.0f;
    state->focus_lost = false;
}

void input_clear_all(input_state_t *state)
{
    ASSERT(state != NULL);

    // 关键：先把 current 快照进 previous，再清空 current。
    //
    // 这样做的效果是"所有当时按着的键，本帧一定会产生一次 released 边沿"，
    // 与失焦发生在帧内哪个时刻无关。如果只清 current 不更新 previous，
    // 那么当"按下"和"失焦"发生在同一帧时，previous 里还是更早的状态（0），
    // released 边沿就不会产生 —— 拖拽、蓄力这类逻辑会永远收不到结束事件。
    memcpy(state->keys_previous, state->keys, sizeof(state->keys));
    memcpy(state->mouse_buttons_previous, state->mouse_buttons, sizeof(state->mouse_buttons));

    memset(state->keys, 0, sizeof(state->keys));
    memset(state->mouse_buttons, 0, sizeof(state->mouse_buttons));
}

// ---------------------------------------------------------------- 事件注入
void input_push_key(input_state_t *state, key_t key, bool down)
{
    ASSERT(state != NULL);

    if (key <= KEY_UNKNOWN || key >= KEY_COUNT) {
        return;
    }
    state->keys[key] = down ? 1u : 0u;
}

void input_push_mouse_button(input_state_t *state, mouse_button_t button, bool down)
{
    ASSERT(state != NULL);

    if (button < 0 || button >= MOUSE_BUTTON_COUNT) {
        return;
    }
    state->mouse_buttons[button] = down ? 1u : 0u;
}

void input_push_mouse_move(input_state_t *state, f32 x, f32 y)
{
    ASSERT(state != NULL);
    state->mouse_x = x;
    state->mouse_y = y;
}

void input_push_mouse_delta(input_state_t *state, f32 dx, f32 dy)
{
    ASSERT(state != NULL);
    state->mouse_dx += dx;
    state->mouse_dy += dy;
}

void input_push_scroll(input_state_t *state, f32 dx, f32 dy)
{
    ASSERT(state != NULL);
    state->scroll_dx += dx;
    state->scroll_dy += dy;
}

// ---------------------------------------------------------------- 查询
bool input_key_down(const input_state_t *state, key_t key)
{
    if (state == NULL || key <= KEY_UNKNOWN || key >= KEY_COUNT) {
        return false;
    }
    return state->keys[key] != 0;
}

bool input_key_pressed(const input_state_t *state, key_t key)
{
    if (state == NULL || key <= KEY_UNKNOWN || key >= KEY_COUNT) {
        return false;
    }
    return state->keys[key] != 0 && state->keys_previous[key] == 0;
}

bool input_key_released(const input_state_t *state, key_t key)
{
    if (state == NULL || key <= KEY_UNKNOWN || key >= KEY_COUNT) {
        return false;
    }
    return state->keys[key] == 0 && state->keys_previous[key] != 0;
}

bool input_mouse_down(const input_state_t *state, mouse_button_t button)
{
    if (state == NULL || button < 0 || button >= MOUSE_BUTTON_COUNT) {
        return false;
    }
    return state->mouse_buttons[button] != 0;
}

bool input_mouse_pressed(const input_state_t *state, mouse_button_t button)
{
    if (state == NULL || button < 0 || button >= MOUSE_BUTTON_COUNT) {
        return false;
    }
    return state->mouse_buttons[button] != 0 && state->mouse_buttons_previous[button] == 0;
}

bool input_mouse_released(const input_state_t *state, mouse_button_t button)
{
    if (state == NULL || button < 0 || button >= MOUSE_BUTTON_COUNT) {
        return false;
    }
    return state->mouse_buttons[button] == 0 && state->mouse_buttons_previous[button] != 0;
}

bool input_any_key_pressed(const input_state_t *state)
{
    if (state == NULL) {
        return false;
    }
    for (u32 i = 1; i < KEY_COUNT; ++i) {
        if (state->keys[i] != 0 && state->keys_previous[i] == 0) {
            return true;
        }
    }
    return false;
}

// =============================================================================
//  Win32 虚拟键码 -> key_t
//
//  用"设计初始化器"写这张表：没列出的虚拟键码默认是 0（KEY_UNKNOWN）。
//  这张表是**唯一事实来源** —— 反向表（key_t -> VK）在首次使用时由它构建，
//  这样不会出现"正向表和反向表对不上"的问题。
// =============================================================================
static const key_t s_vk_to_key[256] = {
    [0x08] = KEY_BACKSPACE,
    [0x09] = KEY_TAB,
    [0x0D] = KEY_ENTER,
    [0x13] = KEY_PAUSE,
    [0x14] = KEY_CAPS_LOCK,
    [0x1B] = KEY_ESCAPE,
    [0x20] = KEY_SPACE,
    [0x21] = KEY_PAGE_UP,
    [0x22] = KEY_PAGE_DOWN,
    [0x23] = KEY_END,
    [0x24] = KEY_HOME,
    [0x25] = KEY_LEFT,
    [0x26] = KEY_UP,
    [0x27] = KEY_RIGHT,
    [0x28] = KEY_DOWN,
    [0x2C] = KEY_PRINT_SCREEN,
    [0x2D] = KEY_INSERT,
    [0x2E] = KEY_DELETE,

    [0x30] = KEY_0, [0x31] = KEY_1, [0x32] = KEY_2, [0x33] = KEY_3, [0x34] = KEY_4,
    [0x35] = KEY_5, [0x36] = KEY_6, [0x37] = KEY_7, [0x38] = KEY_8, [0x39] = KEY_9,

    [0x41] = KEY_A, [0x42] = KEY_B, [0x43] = KEY_C, [0x44] = KEY_D, [0x45] = KEY_E,
    [0x46] = KEY_F, [0x47] = KEY_G, [0x48] = KEY_H, [0x49] = KEY_I, [0x4A] = KEY_J,
    [0x4B] = KEY_K, [0x4C] = KEY_L, [0x4D] = KEY_M, [0x4E] = KEY_N, [0x4F] = KEY_O,
    [0x50] = KEY_P, [0x51] = KEY_Q, [0x52] = KEY_R, [0x53] = KEY_S, [0x54] = KEY_T,
    [0x55] = KEY_U, [0x56] = KEY_V, [0x57] = KEY_W, [0x58] = KEY_X, [0x59] = KEY_Y,
    [0x5A] = KEY_Z,

    [0x5B] = KEY_LEFT_SUPER,
    [0x5C] = KEY_RIGHT_SUPER,

    [0x60] = KEY_NUMPAD_0, [0x61] = KEY_NUMPAD_1, [0x62] = KEY_NUMPAD_2,
    [0x63] = KEY_NUMPAD_3, [0x64] = KEY_NUMPAD_4, [0x65] = KEY_NUMPAD_5,
    [0x66] = KEY_NUMPAD_6, [0x67] = KEY_NUMPAD_7, [0x68] = KEY_NUMPAD_8,
    [0x69] = KEY_NUMPAD_9,
    [0x6A] = KEY_NUMPAD_MULTIPLY,
    [0x6B] = KEY_NUMPAD_ADD,
    [0x6D] = KEY_NUMPAD_SUBTRACT,
    [0x6E] = KEY_NUMPAD_DECIMAL,
    [0x6F] = KEY_NUMPAD_DIVIDE,

    [0x70] = KEY_F1,  [0x71] = KEY_F2,  [0x72] = KEY_F3,  [0x73] = KEY_F4,
    [0x74] = KEY_F5,  [0x75] = KEY_F6,  [0x76] = KEY_F7,  [0x77] = KEY_F8,
    [0x78] = KEY_F9,  [0x79] = KEY_F10, [0x7A] = KEY_F11, [0x7B] = KEY_F12,

    [0x90] = KEY_NUM_LOCK,
    [0x91] = KEY_SCROLL_LOCK,

    [0xA0] = KEY_LEFT_SHIFT,  [0xA1] = KEY_RIGHT_SHIFT,
    [0xA2] = KEY_LEFT_CTRL,   [0xA3] = KEY_RIGHT_CTRL,
    [0xA4] = KEY_LEFT_ALT,    [0xA5] = KEY_RIGHT_ALT,

    [0xBA] = KEY_SEMICOLON,
    [0xBB] = KEY_EQUAL,
    [0xBC] = KEY_COMMA,
    [0xBD] = KEY_MINUS,
    [0xBE] = KEY_PERIOD,
    [0xBF] = KEY_SLASH,
    [0xC0] = KEY_GRAVE,
    [0xDB] = KEY_LEFT_BRACKET,
    [0xDC] = KEY_BACKSLASH,
    [0xDD] = KEY_RIGHT_BRACKET,
    [0xDE] = KEY_APOSTROPHE,
};

// 反向表。首次使用时从 s_vk_to_key 构建，保证两者永远一致。
static u16  s_key_to_vk[KEY_COUNT];
static bool s_key_to_vk_ready = false;

static void build_reverse_table(void)
{
    if (s_key_to_vk_ready) {
        return;
    }
    memset(s_key_to_vk, 0, sizeof(s_key_to_vk));
    for (u32 vk = 1; vk < 256; ++vk) {
        const key_t key = s_vk_to_key[vk];
        if (key != KEY_UNKNOWN) {
            s_key_to_vk[key] = (u16)vk;
        }
    }
    s_key_to_vk_ready = true;
}

key_t platform_input_key_from_native(u32 native_key, u32 native_scancode, bool extended)
{
    u32 vk = native_key & 0xFFu;

    // WM_KEYDOWN 对修饰键给的是**通用**虚拟键码（VK_SHIFT / VK_CONTROL / VK_MENU），
    // 不区分左右。要区分必须借助扫描码或 extended 标志：
    //   * Shift 左右靠扫描码（0x2A 左 / 0x36 右），MapVirtualKey 的
    //     MAPVK_VSC_TO_VK_EX 专门干这个；
    //   * Ctrl / Alt 左右靠 extended 标志（右 Ctrl、右 Alt 是扩展键）。
    if (vk == VK_SHIFT) {
        const UINT mapped = MapVirtualKeyW(native_scancode & 0xFFu, MAPVK_VSC_TO_VK_EX);
        if (mapped == VK_LSHIFT || mapped == VK_RSHIFT) {
            vk = mapped;
        } else {
            vk = VK_LSHIFT; // 拿不到就默认左，至少不会丢事件
        }
    } else if (vk == VK_CONTROL) {
        vk = extended ? VK_RCONTROL : VK_LCONTROL;
    } else if (vk == VK_MENU) {
        vk = extended ? VK_RMENU : VK_LMENU;
    } else if (vk == VK_RETURN && extended) {
        // 小键盘回车和小键盘除号在系统里是"扩展键"，
        // 主键盘回车不带 extended 标志。不区分的话小键盘回车会被
        // 当成主回车，重绑定界面上两个键会互相覆盖。
        return KEY_NUMPAD_ENTER;
    }

    return s_vk_to_key[vk & 0xFFu];
}

// ---------------------------------------------------------------- 名字
static const char *const s_key_names[KEY_COUNT] = {
    [KEY_UNKNOWN] = "Unknown",
    [KEY_A] = "A", [KEY_B] = "B", [KEY_C] = "C", [KEY_D] = "D", [KEY_E] = "E",
    [KEY_F] = "F", [KEY_G] = "G", [KEY_H] = "H", [KEY_I] = "I", [KEY_J] = "J",
    [KEY_K] = "K", [KEY_L] = "L", [KEY_M] = "M", [KEY_N] = "N", [KEY_O] = "O",
    [KEY_P] = "P", [KEY_Q] = "Q", [KEY_R] = "R", [KEY_S] = "S", [KEY_T] = "T",
    [KEY_U] = "U", [KEY_V] = "V", [KEY_W] = "W", [KEY_X] = "X", [KEY_Y] = "Y",
    [KEY_Z] = "Z",
    [KEY_0] = "0", [KEY_1] = "1", [KEY_2] = "2", [KEY_3] = "3", [KEY_4] = "4",
    [KEY_5] = "5", [KEY_6] = "6", [KEY_7] = "7", [KEY_8] = "8", [KEY_9] = "9",
    [KEY_F1] = "F1",   [KEY_F2] = "F2",   [KEY_F3] = "F3",   [KEY_F4] = "F4",
    [KEY_F5] = "F5",   [KEY_F6] = "F6",   [KEY_F7] = "F7",   [KEY_F8] = "F8",
    [KEY_F9] = "F9",   [KEY_F10] = "F10", [KEY_F11] = "F11", [KEY_F12] = "F12",
    [KEY_ESCAPE] = "Escape", [KEY_ENTER] = "Enter", [KEY_TAB] = "Tab",
    [KEY_SPACE] = "Space", [KEY_BACKSPACE] = "Backspace",
    [KEY_INSERT] = "Insert", [KEY_DELETE] = "Delete", [KEY_HOME] = "Home",
    [KEY_END] = "End", [KEY_PAGE_UP] = "PageUp", [KEY_PAGE_DOWN] = "PageDown",
    [KEY_LEFT] = "Left", [KEY_RIGHT] = "Right", [KEY_UP] = "Up", [KEY_DOWN] = "Down",
    [KEY_LEFT_SHIFT] = "LeftShift", [KEY_RIGHT_SHIFT] = "RightShift",
    [KEY_LEFT_CTRL] = "LeftCtrl", [KEY_RIGHT_CTRL] = "RightCtrl",
    [KEY_LEFT_ALT] = "LeftAlt", [KEY_RIGHT_ALT] = "RightAlt",
    [KEY_LEFT_SUPER] = "LeftSuper", [KEY_RIGHT_SUPER] = "RightSuper",
    [KEY_MINUS] = "Minus", [KEY_EQUAL] = "Equal",
    [KEY_LEFT_BRACKET] = "LeftBracket", [KEY_RIGHT_BRACKET] = "RightBracket",
    [KEY_BACKSLASH] = "Backslash", [KEY_SEMICOLON] = "Semicolon",
    [KEY_APOSTROPHE] = "Apostrophe", [KEY_GRAVE] = "Grave",
    [KEY_COMMA] = "Comma", [KEY_PERIOD] = "Period", [KEY_SLASH] = "Slash",
    [KEY_CAPS_LOCK] = "CapsLock", [KEY_NUM_LOCK] = "NumLock",
    [KEY_SCROLL_LOCK] = "ScrollLock",
    [KEY_PRINT_SCREEN] = "PrintScreen", [KEY_PAUSE] = "Pause",
    [KEY_NUMPAD_0] = "Numpad0", [KEY_NUMPAD_1] = "Numpad1", [KEY_NUMPAD_2] = "Numpad2",
    [KEY_NUMPAD_3] = "Numpad3", [KEY_NUMPAD_4] = "Numpad4", [KEY_NUMPAD_5] = "Numpad5",
    [KEY_NUMPAD_6] = "Numpad6", [KEY_NUMPAD_7] = "Numpad7", [KEY_NUMPAD_8] = "Numpad8",
    [KEY_NUMPAD_9] = "Numpad9",
    [KEY_NUMPAD_DECIMAL] = "NumpadDecimal", [KEY_NUMPAD_DIVIDE] = "NumpadDivide",
    [KEY_NUMPAD_MULTIPLY] = "NumpadMultiply", [KEY_NUMPAD_SUBTRACT] = "NumpadSubtract",
    [KEY_NUMPAD_ADD] = "NumpadAdd", [KEY_NUMPAD_ENTER] = "NumpadEnter",
};

const char *input_key_name(key_t key)
{
    if (key < 0 || key >= KEY_COUNT) {
        return "Unknown";
    }
    const char *name = s_key_names[key];
    return (name != NULL) ? name : "Unknown";
}

const char *input_mouse_button_name(mouse_button_t button)
{
    switch (button) {
    case MOUSE_BUTTON_LEFT:
        return "Left";
    case MOUSE_BUTTON_RIGHT:
        return "Right";
    case MOUSE_BUTTON_MIDDLE:
        return "Middle";
    case MOUSE_BUTTON_X1:
        return "X1";
    case MOUSE_BUTTON_X2:
        return "X2";
    default:
        return "Unknown";
    }
}

// 这个函数目前只被测试与将来的输入重绑定界面使用。
// 保留它是为了确保反向表被构建 —— 否则 s_key_to_vk 永远是空的。
u32 platform_input_native_from_key(key_t key)
{
    if (key <= KEY_UNKNOWN || key >= KEY_COUNT) {
        return 0;
    }
    build_reverse_table();
    return (u32)s_key_to_vk[key];
}
