// =============================================================================
//  tests/test_input.c —— 输入状态机与按键映射
//
//  输入状态机被刻意写成不依赖窗口的形式（事件靠 input_push_* 注入），
//  所以这里可以脱离消息循环构造任意输入序列。
//  "边沿检测对不对"是这类代码最容易出错的地方 —— 用真实窗口测既慢又不稳定。
// =============================================================================
#include "test_framework.h"
#include "test_suites.h"

static input_state_t make_input(void)
{
    input_state_t input;
    memset(&input, 0, sizeof(input));
    return input;
}

// ---------------------------------------------------------------- 按键边沿
static void key_press_hold_release_lifecycle(void)
{
    input_state_t input = make_input();

    // 第 1 帧：按下
    input_begin_frame(&input);
    input_push_key(&input, KEY_SPACE, true);

    EXPECT_TRUE(input_key_down(&input, KEY_SPACE));
    EXPECT_TRUE(input_key_pressed(&input, KEY_SPACE));
    EXPECT_FALSE(input_key_released(&input, KEY_SPACE));

    // 第 2 帧：持续按住 —— 不应该再报"刚按下"。
    // 这一条如果错了，"按住就连续跳"这类 bug 就出现了。
    input_begin_frame(&input);
    EXPECT_TRUE(input_key_down(&input, KEY_SPACE));
    EXPECT_FALSE(input_key_pressed(&input, KEY_SPACE));
    EXPECT_FALSE(input_key_released(&input, KEY_SPACE));

    // 第 3 帧：松开
    input_begin_frame(&input);
    input_push_key(&input, KEY_SPACE, false);
    EXPECT_FALSE(input_key_down(&input, KEY_SPACE));
    EXPECT_FALSE(input_key_pressed(&input, KEY_SPACE));
    EXPECT_TRUE(input_key_released(&input, KEY_SPACE));

    // 第 4 帧：不再"刚松开"
    input_begin_frame(&input);
    EXPECT_FALSE(input_key_released(&input, KEY_SPACE));
}

static void press_and_release_within_one_frame_is_lost(void)
{
    // 双缓冲的固有局限，记录在测试里以免将来有人误以为它工作。
    // 一帧内"按下又松开"最终状态是"没按下"，而 previous 也是"没按下"，
    // 所以两个边沿都检测不到。
    // 需要"瞬间点按"的场景（菜单、连点）应当靠 M9 的输入事件队列解决，
    // 而不是指望双缓冲。
    input_state_t input = make_input();

    input_begin_frame(&input);
    input_push_key(&input, KEY_A, true);
    input_push_key(&input, KEY_A, false);

    EXPECT_FALSE(input_key_down(&input, KEY_A));
    EXPECT_FALSE(input_key_pressed(&input, KEY_A));

    input_begin_frame(&input);
    EXPECT_FALSE(input_key_pressed(&input, KEY_A));
    EXPECT_FALSE(input_key_released(&input, KEY_A));
}

static void multiple_keys_are_independent(void)
{
    input_state_t input = make_input();

    input_begin_frame(&input);
    input_push_key(&input, KEY_W, true);
    input_push_key(&input, KEY_LEFT_SHIFT, true);

    EXPECT_TRUE(input_key_down(&input, KEY_W));
    EXPECT_TRUE(input_key_down(&input, KEY_LEFT_SHIFT));
    EXPECT_FALSE(input_key_down(&input, KEY_S));

    input_begin_frame(&input);
    input_push_key(&input, KEY_W, false);
    input_push_key(&input, KEY_LEFT_SHIFT, true);

    EXPECT_FALSE(input_key_down(&input, KEY_W));
    EXPECT_TRUE(input_key_released(&input, KEY_W));
    EXPECT_TRUE(input_key_down(&input, KEY_LEFT_SHIFT));
    EXPECT_FALSE(input_key_pressed(&input, KEY_LEFT_SHIFT));
}

static void any_key_pressed_detects_first_press(void)
{
    input_state_t input = make_input();

    input_begin_frame(&input);
    EXPECT_FALSE(input_any_key_pressed(&input));

    input_push_key(&input, KEY_F1, true);
    EXPECT_TRUE(input_any_key_pressed(&input));

    input_begin_frame(&input);
    EXPECT_FALSE(input_any_key_pressed(&input)); // 按住不算"刚按下"

    input_push_key(&input, KEY_F1, true); // 仍然是按住
    EXPECT_FALSE(input_any_key_pressed(&input));

    input_push_key(&input, KEY_F2, true);
    EXPECT_TRUE(input_any_key_pressed(&input));
}

// ---------------------------------------------------------------- 鼠标
static void mouse_button_edge_detection(void)
{
    input_state_t input = make_input();

    input_begin_frame(&input);
    input_push_mouse_button(&input, MOUSE_BUTTON_LEFT, true);
    EXPECT_TRUE(input_mouse_down(&input, MOUSE_BUTTON_LEFT));
    EXPECT_TRUE(input_mouse_pressed(&input, MOUSE_BUTTON_LEFT));
    EXPECT_FALSE(input_mouse_released(&input, MOUSE_BUTTON_LEFT));

    input_begin_frame(&input);
    EXPECT_TRUE(input_mouse_down(&input, MOUSE_BUTTON_LEFT));
    EXPECT_FALSE(input_mouse_pressed(&input, MOUSE_BUTTON_LEFT));

    input_begin_frame(&input);
    input_push_mouse_button(&input, MOUSE_BUTTON_LEFT, false);
    EXPECT_FALSE(input_mouse_down(&input, MOUSE_BUTTON_LEFT));
    EXPECT_TRUE(input_mouse_released(&input, MOUSE_BUTTON_LEFT));

    // 各按键互不影响
    input_begin_frame(&input);
    input_push_mouse_button(&input, MOUSE_BUTTON_RIGHT, true);
    EXPECT_TRUE(input_mouse_down(&input, MOUSE_BUTTON_RIGHT));
    EXPECT_FALSE(input_mouse_down(&input, MOUSE_BUTTON_LEFT));
    EXPECT_FALSE(input_mouse_down(&input, MOUSE_BUTTON_MIDDLE));
}

static void mouse_delta_accumulates_then_resets(void)
{
    input_state_t input = make_input();

    input_begin_frame(&input);
    // 一帧内可能收到多条 WM_MOUSEMOVE，位移必须累加而不是覆盖
    input_push_mouse_delta(&input, 1.0f, 2.0f);
    input_push_mouse_delta(&input, 3.0f, 4.0f);
    input_push_mouse_delta(&input, -0.5f, 0.5f);

    EXPECT_NEAR(input.mouse_dx, 3.5f, 0.0f);
    EXPECT_NEAR(input.mouse_dy, 6.5f, 0.0f);

    // 下一帧必须清零，否则位移会一直累加下去
    input_begin_frame(&input);
    EXPECT_NEAR(input.mouse_dx, 0.0f, 0.0f);
    EXPECT_NEAR(input.mouse_dy, 0.0f, 0.0f);
}

static void mouse_position_persists_across_frames(void)
{
    input_state_t input = make_input();

    input_begin_frame(&input);
    input_push_mouse_move(&input, 640.0f, 360.0f);

    // 绝对位置是状态量，不该像增量那样被清零
    input_begin_frame(&input);
    EXPECT_NEAR(input.mouse_x, 640.0f, 0.0f);
    EXPECT_NEAR(input.mouse_y, 360.0f, 0.0f);

    input_push_mouse_move(&input, 100.0f, 200.0f);
    EXPECT_NEAR(input.mouse_x, 100.0f, 0.0f);
    EXPECT_NEAR(input.mouse_y, 200.0f, 0.0f);
}

static void scroll_accumulates_and_resets(void)
{
    input_state_t input = make_input();

    input_begin_frame(&input);
    input_push_scroll(&input, 0.0f, 1.0f);
    input_push_scroll(&input, 0.0f, 1.0f);
    input_push_scroll(&input, -1.0f, 0.0f);

    EXPECT_NEAR(input.scroll_dx, -1.0f, 0.0f);
    EXPECT_NEAR(input.scroll_dy, 2.0f, 0.0f);

    input_begin_frame(&input);
    EXPECT_NEAR(input.scroll_dx, 0.0f, 0.0f);
    EXPECT_NEAR(input.scroll_dy, 0.0f, 0.0f);
}

// ---------------------------------------------------------------- 失焦
static void clear_all_releases_held_keys(void)
{
    input_state_t input = make_input();

    // 场景 A：按下之后跨了一帧才失焦（真实情况里最常见）
    input_begin_frame(&input);
    input_push_key(&input, KEY_W, true);
    input_push_key(&input, KEY_LEFT_SHIFT, true);
    input_push_mouse_button(&input, MOUSE_BUTTON_LEFT, true);

    input_begin_frame(&input); // previous 现在记录着"这些键是按着的"
    input_push_key(&input, KEY_W, true);
    input_push_key(&input, KEY_LEFT_SHIFT, true);
    input_push_mouse_button(&input, MOUSE_BUTTON_LEFT, true);
    EXPECT_TRUE(input_key_down(&input, KEY_W));

    input_clear_all(&input);
    input.focus_lost = true;

    EXPECT_FALSE(input_key_down(&input, KEY_W));
    EXPECT_FALSE(input_key_down(&input, KEY_LEFT_SHIFT));
    EXPECT_FALSE(input_mouse_down(&input, MOUSE_BUTTON_LEFT));
    EXPECT_TRUE(input_key_released(&input, KEY_W));
    EXPECT_TRUE(input_mouse_released(&input, MOUSE_BUTTON_LEFT));

    // 场景 B：按下和失焦发生在同一帧。
    // 这里必须也能产生 released —— 实现上 clear_all 会先把 current 快照进
    // previous 再清空，就是为了覆盖这种情况。
    // 不这么做的话，"按住鼠标拖拽时 alt-tab 出去"会让拖拽逻辑永远收不到
    // 结束事件，物品会一直粘在光标上。
    input_state_t same_frame = make_input();
    input_begin_frame(&same_frame);
    input_push_mouse_button(&same_frame, MOUSE_BUTTON_LEFT, true);
    input_clear_all(&same_frame);

    EXPECT_FALSE(input_mouse_down(&same_frame, MOUSE_BUTTON_LEFT));
    EXPECT_TRUE(input_mouse_released(&same_frame, MOUSE_BUTTON_LEFT));
}

static void begin_frame_clears_focus_lost(void)
{
    input_state_t input = make_input();

    input_clear_all(&input);
    input.focus_lost = true;
    EXPECT_TRUE(input.focus_lost);

    input_begin_frame(&input);
    EXPECT_FALSE(input.focus_lost);
}

// ---------------------------------------------------------------- 健壮性
static void queries_tolerate_invalid_input(void)
{
    input_state_t input = make_input();

    EXPECT_FALSE(input_key_down(NULL, KEY_A));
    EXPECT_FALSE(input_key_pressed(NULL, KEY_A));
    EXPECT_FALSE(input_key_released(NULL, KEY_A));
    EXPECT_FALSE(input_mouse_down(NULL, MOUSE_BUTTON_LEFT));
    EXPECT_FALSE(input_any_key_pressed(NULL));

    EXPECT_FALSE(input_key_down(&input, KEY_UNKNOWN));
    EXPECT_FALSE(input_key_down(&input, (key_t)KEY_COUNT));
    EXPECT_FALSE(input_key_down(&input, (key_t)-1));
    EXPECT_FALSE(input_mouse_down(&input, (mouse_button_t)MOUSE_BUTTON_COUNT));
    EXPECT_FALSE(input_mouse_down(&input, (mouse_button_t)-1));

    // 越界的事件注入不能写坏内存
    input_push_key(&input, KEY_UNKNOWN, true);
    input_push_key(&input, (key_t)9999, true);
    input_push_key(&input, (key_t)-1, true);
    input_push_mouse_button(&input, (mouse_button_t)99, true);
    EXPECT_FALSE(input_any_key_pressed(&input));
}

// ---------------------------------------------------------------- 按键映射
static void native_mapping_covers_common_keys(void)
{
    // 直接用字面量而不是 VK_ 宏：测试文件不应该包含 windows.h，
    // 而且字面量能把"期望的映射"钉死，改错了会立刻失败。
    EXPECT_EQ_U64(platform_input_key_from_native(0x41, 0x1E, false), KEY_A); // 'A'
    EXPECT_EQ_U64(platform_input_key_from_native(0x5A, 0x2C, false), KEY_Z); // 'Z'
    EXPECT_EQ_U64(platform_input_key_from_native(0x30, 0x0B, false), KEY_0); // '0'
    EXPECT_EQ_U64(platform_input_key_from_native(0x39, 0x0A, false), KEY_9); // '9'
    EXPECT_EQ_U64(platform_input_key_from_native(0x1B, 0x01, false), KEY_ESCAPE);
    EXPECT_EQ_U64(platform_input_key_from_native(0x0D, 0x1C, false), KEY_ENTER);
    EXPECT_EQ_U64(platform_input_key_from_native(0x20, 0x39, false), KEY_SPACE);
    EXPECT_EQ_U64(platform_input_key_from_native(0x09, 0x0F, false), KEY_TAB);
    EXPECT_EQ_U64(platform_input_key_from_native(0x25, 0x4B, true), KEY_LEFT);
    EXPECT_EQ_U64(platform_input_key_from_native(0x27, 0x4D, true), KEY_RIGHT);
    EXPECT_EQ_U64(platform_input_key_from_native(0x26, 0x48, true), KEY_UP);
    EXPECT_EQ_U64(platform_input_key_from_native(0x28, 0x50, true), KEY_DOWN);
    EXPECT_EQ_U64(platform_input_key_from_native(0x70, 0x3B, false), KEY_F1);
    EXPECT_EQ_U64(platform_input_key_from_native(0x7B, 0x57, false), KEY_F12);

    // 未映射的虚拟键码返回 KEY_UNKNOWN 而不是乱猜
    EXPECT_EQ_U64(platform_input_key_from_native(0xFF, 0x00, false), KEY_UNKNOWN);
}

static void modifier_left_right_are_distinguished(void)
{
    // Shift 的左右靠扫描码区分（0x2A 左、0x36 右）
    EXPECT_EQ_U64(platform_input_key_from_native(0x10, 0x2A, false), KEY_LEFT_SHIFT);
    EXPECT_EQ_U64(platform_input_key_from_native(0x10, 0x36, false), KEY_RIGHT_SHIFT);

    // Ctrl / Alt 的左右靠 extended 标志区分
    EXPECT_EQ_U64(platform_input_key_from_native(0x11, 0x1D, false), KEY_LEFT_CTRL);
    EXPECT_EQ_U64(platform_input_key_from_native(0x11, 0x1D, true), KEY_RIGHT_CTRL);
    EXPECT_EQ_U64(platform_input_key_from_native(0x12, 0x38, false), KEY_LEFT_ALT);
    EXPECT_EQ_U64(platform_input_key_from_native(0x12, 0x38, true), KEY_RIGHT_ALT);
}

static void numpad_enter_is_not_main_enter(void)
{
    // 小键盘回车是"扩展键"，主键盘回车不是。
    // 不区分的话两者会互相覆盖，重绑定界面会出现"按了 A 却显示 B"。
    EXPECT_EQ_U64(platform_input_key_from_native(0x0D, 0x1C, false), KEY_ENTER);
    EXPECT_EQ_U64(platform_input_key_from_native(0x0D, 0x1C, true), KEY_NUMPAD_ENTER);
}

static void native_mapping_round_trips(void)
{
    // 正向表与反向表必须一致。两张表各写一遍是"按 W 触发 S"这类 bug 的温床，
    // 所以实现里反向表是从正向表推导出来的，这里验证推导结果正确。
    const key_t keys[] = {
        KEY_A, KEY_Z, KEY_0, KEY_9, KEY_F1, KEY_F12, KEY_ESCAPE, KEY_ENTER,
        KEY_TAB, KEY_SPACE, KEY_BACKSPACE, KEY_LEFT, KEY_RIGHT, KEY_UP, KEY_DOWN,
        KEY_LEFT_SHIFT, KEY_RIGHT_SHIFT, KEY_LEFT_CTRL, KEY_RIGHT_CTRL,
        KEY_LEFT_ALT, KEY_RIGHT_ALT, KEY_LEFT_SUPER, KEY_RIGHT_SUPER,
        KEY_MINUS, KEY_EQUAL, KEY_COMMA, KEY_PERIOD, KEY_SLASH, KEY_SEMICOLON,
        KEY_APOSTROPHE, KEY_GRAVE, KEY_LEFT_BRACKET, KEY_RIGHT_BRACKET, KEY_BACKSLASH,
        KEY_NUMPAD_0, KEY_NUMPAD_9, KEY_NUMPAD_ADD, KEY_NUMPAD_SUBTRACT,
        KEY_NUMPAD_MULTIPLY, KEY_NUMPAD_DIVIDE, KEY_NUMPAD_DECIMAL,
        KEY_CAPS_LOCK, KEY_NUM_LOCK, KEY_SCROLL_LOCK, KEY_PRINT_SCREEN, KEY_PAUSE,
        KEY_INSERT, KEY_DELETE, KEY_HOME, KEY_END, KEY_PAGE_UP, KEY_PAGE_DOWN,
    };

    for (usize i = 0; i < ENGINE_ARRAY_COUNT(keys); ++i) {
        const u32 native = platform_input_native_from_key(keys[i]);
        EXPECT_GT_U64(native, 0u);
        if (native == 0) {
            continue;
        }
        EXPECT_EQ_U64(platform_input_key_from_native(native, 0, false), keys[i]);
    }
}

static void reverse_mapping_rejects_invalid(void)
{
    EXPECT_EQ_U64(platform_input_native_from_key(KEY_UNKNOWN), 0u);
    EXPECT_EQ_U64(platform_input_native_from_key((key_t)KEY_COUNT), 0u);
    EXPECT_EQ_U64(platform_input_native_from_key((key_t)-1), 0u);
    // 小键盘回车没有独立的虚拟键码，反向映射拿不到，返回 0 是正确的
    EXPECT_EQ_U64(platform_input_native_from_key(KEY_NUMPAD_ENTER), 0u);
}

static void every_key_has_a_name(void)
{
    for (u32 i = 0; i < KEY_COUNT; ++i) {
        const char *name = input_key_name((key_t)i);
        EXPECT_NOT_NULL(name);
        EXPECT_GT_U64(strlen(name), 0u);
    }

    EXPECT_STR_EQ(input_key_name(KEY_A), "A");
    EXPECT_STR_EQ(input_key_name(KEY_ESCAPE), "Escape");
    EXPECT_STR_EQ(input_key_name(KEY_PAGE_UP), "PageUp");
    EXPECT_STR_EQ(input_key_name(KEY_NUMPAD_ADD), "NumpadAdd");
    EXPECT_STR_EQ(input_key_name(KEY_UNKNOWN), "Unknown");
    EXPECT_STR_EQ(input_key_name((key_t)9999), "Unknown");

    EXPECT_STR_EQ(input_mouse_button_name(MOUSE_BUTTON_LEFT), "Left");
    EXPECT_STR_EQ(input_mouse_button_name(MOUSE_BUTTON_X2), "X2");
    EXPECT_STR_EQ(input_mouse_button_name((mouse_button_t)99), "Unknown");
}

static void key_enum_has_no_duplicate_values(void)
{
    // 枚举值重复会导致两个键共享同一个状态槽位（"按 A 和 B 效果一样"）。
    // 这种错误手写枚举时很容易犯。
    EXPECT_EQ_U64(KEY_UNKNOWN, 0u);
    EXPECT_GT_U64(KEY_COUNT, 90u);

    // 抽查几个容易写重的位置
    EXPECT_TRUE(KEY_A != KEY_B);
    EXPECT_TRUE(KEY_LEFT_SHIFT != KEY_LEFT_CTRL);
    EXPECT_TRUE(KEY_0 != KEY_NUMPAD_0);
    EXPECT_TRUE(KEY_ENTER != KEY_NUMPAD_ENTER);
    EXPECT_TRUE(KEY_F1 != KEY_1);
}

void test_input_register(void)
{
    test_register("input", "key_press_hold_release_lifecycle", key_press_hold_release_lifecycle);
    test_register("input", "press_and_release_within_one_frame_is_lost",
                  press_and_release_within_one_frame_is_lost);
    test_register("input", "multiple_keys_are_independent", multiple_keys_are_independent);
    test_register("input", "any_key_pressed_detects_first_press", any_key_pressed_detects_first_press);

    test_register("input", "mouse_button_edge_detection", mouse_button_edge_detection);
    test_register("input", "mouse_delta_accumulates_then_resets", mouse_delta_accumulates_then_resets);
    test_register("input", "mouse_position_persists_across_frames",
                  mouse_position_persists_across_frames);
    test_register("input", "scroll_accumulates_and_resets", scroll_accumulates_and_resets);

    test_register("input", "clear_all_releases_held_keys", clear_all_releases_held_keys);
    test_register("input", "begin_frame_clears_focus_lost", begin_frame_clears_focus_lost);
    test_register("input", "queries_tolerate_invalid_input", queries_tolerate_invalid_input);

    test_register("input", "native_mapping_covers_common_keys", native_mapping_covers_common_keys);
    test_register("input", "modifier_left_right_are_distinguished",
                  modifier_left_right_are_distinguished);
    test_register("input", "numpad_enter_is_not_main_enter", numpad_enter_is_not_main_enter);
    test_register("input", "native_mapping_round_trips", native_mapping_round_trips);
    test_register("input", "reverse_mapping_rejects_invalid", reverse_mapping_rejects_invalid);
    test_register("input", "every_key_has_a_name", every_key_has_a_name);
    test_register("input", "key_enum_has_no_duplicate_values", key_enum_has_no_duplicate_values);
}
