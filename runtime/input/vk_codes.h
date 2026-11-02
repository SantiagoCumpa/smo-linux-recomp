/* Key codes of the runtime's keyboard configuration (controllers.ini, SMO_AUTOKEY, the hotkeys): the Windows
 * virtual-key numbers the files have always used ('A'..'Z', '0'..'9' and the VK_* below), mapped to SDL scancodes by
 * vk_to_scancode (display.c) to read the keyboard. */
#ifndef SMO_VK_CODES_H
#define SMO_VK_CODES_H
#define VK_BACK 0x08
#define VK_TAB 0x09
#define VK_RETURN 0x0D
#define VK_SHIFT 0x10
#define VK_CONTROL 0x11
#define VK_MENU 0x12
#define VK_ESCAPE 0x1B
#define VK_SPACE 0x20
#define VK_LEFT 0x25
#define VK_UP 0x26
#define VK_RIGHT 0x27
#define VK_DOWN 0x28
#define VK_NUMPAD0 0x60
#define VK_F1 0x70
#define VK_F11 0x7A
#define VK_F12 0x7B
#define VK_LSHIFT 0xA0
#define VK_RSHIFT 0xA1
#define VK_LCONTROL 0xA2
#define VK_RCONTROL 0xA3
#define VK_LMENU 0xA4
#define VK_RMENU 0xA5
int vk_to_scancode(int vk);               /* display.c: SDL_Scancode, 0 when the code has no key */
int vk_from_scancode(int scancode);       /* display.c: the code of an SDL scancode, 0 when none */
int display_key_down(int vk);             /* display.c: the key is held (real keyboard while the window has the focus, or a synthetic test key) */
#endif
