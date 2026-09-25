// =============================================================================
//  engine/platform/input.h —— 输入状态
//
//  ---- 双缓冲设计（重要） ----
//
//  输入状态保存两份：current 与 previous。每帧开头把 current 拷进 previous，
//  然后消息循环往里写 current。
//
//  为什么必须两份：
//   1. 边沿检测。要区分"按住"和"这一帧刚按下"，必须知道上一帧的状态。
//      只存一份的话，`key_down` 永远为真，"跳跃"会变成"按住就连续跳"。
//   2. 一帧内多次事件。按住 A 再松开，一帧内会收到 KEYDOWN + KEYUP，
//      只存最终状态的话这一帧看起来"从没按过"。
//
//  ---- 卡键问题 ----
//
//  窗口失去焦点时，系统不会补发 KEYUP。如果不管，玩家 alt-tab 出去再回来，
//  那个键会永远是"按下"状态。所以 WM_KILLFOCUS 时必须调用 input_clear_all()，
//  并把 focus_lost 置位让游戏逻辑知道该暂停。
// =============================================================================
#ifndef ENGINE_PLATFORM_INPUT_H
#define ENGINE_PLATFORM_INPUT_H

#include "engine/core/types.h"

// ---------------------------------------------------------------- 按键
//
// 刻意不使用 Win32 的虚拟键码作为枚举值：那样平台细节会渗进每一处游戏逻辑，
// 将来接 Linux/macOS 时全都要改。这里定义引擎自己的键位，映射表放在
// src/platform/win32/win32_input.c 里。
typedef enum key_t {
    KEY_UNKNOWN = 0,

    KEY_A, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G, KEY_H, KEY_I, KEY_J,
    KEY_K, KEY_L, KEY_M, KEY_N, KEY_O, KEY_P, KEY_Q, KEY_R, KEY_S, KEY_T,
    KEY_U, KEY_V, KEY_W, KEY_X, KEY_Y, KEY_Z,

    KEY_0, KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9,

    KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6,
    KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_F11, KEY_F12,

    KEY_ESCAPE, KEY_ENTER, KEY_TAB, KEY_SPACE, KEY_BACKSPACE,
    KEY_INSERT, KEY_DELETE, KEY_HOME, KEY_END, KEY_PAGE_UP, KEY_PAGE_DOWN,

    KEY_LEFT, KEY_RIGHT, KEY_UP, KEY_DOWN,

    KEY_LEFT_SHIFT, KEY_RIGHT_SHIFT,
    KEY_LEFT_CTRL, KEY_RIGHT_CTRL,
    KEY_LEFT_ALT, KEY_RIGHT_ALT,
    KEY_LEFT_SUPER, KEY_RIGHT_SUPER,

    KEY_MINUS, KEY_EQUAL,
    KEY_LEFT_BRACKET, KEY_RIGHT_BRACKET, KEY_BACKSLASH,
    KEY_SEMICOLON, KEY_APOSTROPHE, KEY_GRAVE,
    KEY_COMMA, KEY_PERIOD, KEY_SLASH,

    KEY_CAPS_LOCK, KEY_NUM_LOCK, KEY_SCROLL_LOCK,
    KEY_PRINT_SCREEN, KEY_PAUSE,

    KEY_NUMPAD_0, KEY_NUMPAD_1, KEY_NUMPAD_2, KEY_NUMPAD_3, KEY_NUMPAD_4,
    KEY_NUMPAD_5, KEY_NUMPAD_6, KEY_NUMPAD_7, KEY_NUMPAD_8, KEY_NUMPAD_9,
    KEY_NUMPAD_DECIMAL, KEY_NUMPAD_DIVIDE, KEY_NUMPAD_MULTIPLY,
    KEY_NUMPAD_SUBTRACT, KEY_NUMPAD_ADD, KEY_NUMPAD_ENTER,

    KEY_COUNT
} key_t;

typedef enum mouse_button_t {
    MOUSE_BUTTON_LEFT = 0,
    MOUSE_BUTTON_RIGHT,
    MOUSE_BUTTON_MIDDLE,
    MOUSE_BUTTON_X1,
    MOUSE_BUTTON_X2,
    MOUSE_BUTTON_COUNT
} mouse_button_t;

// ---------------------------------------------------------------- 状态
typedef struct input_state_t {
    u8 keys[KEY_COUNT];
    u8 keys_previous[KEY_COUNT];

    u8 mouse_buttons[MOUSE_BUTTON_COUNT];
    u8 mouse_buttons_previous[MOUSE_BUTTON_COUNT];

    f32 mouse_x; // 客户区坐标，单位像素，原点左上
    f32 mouse_y;
    f32 mouse_dx; // 本帧累计位移（锁定鼠标时是无限位移，不受窗口边界限制）
    f32 mouse_dy;

    f32 scroll_dx; // 本帧累计滚轮（正值 = 向右/向上滚）
    f32 scroll_dy;

    bool quit_requested;
    bool focus_lost; // 本帧发生了失焦（游戏逻辑通常据此暂停）
} input_state_t;

// ---------------------------------------------------------------- 帧驱动
//
// 调用顺序（由 platform_window_poll_events 负责）：
//     input_begin_frame(&state);   // 帧首：previous <- current，清空本帧增量
//     ... 消息循环往里推事件 ...
//     游戏逻辑查询 input_key_pressed / mouse_dx 等
void input_begin_frame(input_state_t *state);

// 清空所有按下状态。失焦时调用，防止卡键。
//
// 语义保证：调用前处于"按下"状态的键与鼠标键，调用后本帧一定会产生一次
// released 边沿（实现上是先把 current 快照进 previous 再清空）。
// 这让拖拽、蓄力这类依赖"结束事件"的逻辑不会因为失焦时机而漏掉收尾。
void input_clear_all(input_state_t *state);

// ---------------------------------------------------------------- 查询
bool input_key_down(const input_state_t *state, key_t key);
bool input_key_pressed(const input_state_t *state, key_t key);
bool input_key_released(const input_state_t *state, key_t key);

bool input_mouse_down(const input_state_t *state, mouse_button_t button);
bool input_mouse_pressed(const input_state_t *state, mouse_button_t button);
bool input_mouse_released(const input_state_t *state, mouse_button_t button);

// 任意键是否在本帧被按下（用于"按任意键继续"）。
bool input_any_key_pressed(const input_state_t *state);

// ---------------------------------------------------------------- 事件注入
//
// 这两个函数是公开的，不只是内部接口。用途：
//   * 平台层（win32_window.c）从 WndProc 往状态里推事件；
//   * 单元测试可以脱离窗口构造任意输入序列；
//   * M9 的"输入录制/回放"直接把录下来的事件序列重放进来即可。
void input_push_key(input_state_t *state, key_t key, bool down);
void input_push_mouse_button(input_state_t *state, mouse_button_t button, bool down);
void input_push_mouse_move(input_state_t *state, f32 x, f32 y);
void input_push_mouse_delta(input_state_t *state, f32 dx, f32 dy);
void input_push_scroll(input_state_t *state, f32 dx, f32 dy);

// ---------------------------------------------------------------- 平台映射
//
// 把平台相关的按键标识翻译成 key_t。
// native_key / native_scancode 的具体含义由平台决定：
//   Win32: native_key = 虚拟键码（VK_*），native_scancode = lParam 的位 16-23，
//          另外需要 extended 标志区分左右 Ctrl/Alt（见实现）。
// 公开它主要是为了可测试：映射表错了会导致"按 W 却触发了 S"这类问题。
key_t platform_input_key_from_native(u32 native_key, u32 native_scancode, bool extended);

// 反向映射：key_t -> 平台按键标识。主要供测试验证"正反两张表一致"，
// 以及 M9 的输入重绑定界面显示当前绑定的物理键。
// 无法映射时返回 0。
u32 platform_input_native_from_key(key_t key);

// 按键的可读名字，用于日志与重绑定界面。未知键返回 "Unknown"。
const char *input_key_name(key_t key);
const char *input_mouse_button_name(mouse_button_t button);

#endif // ENGINE_PLATFORM_INPUT_H
